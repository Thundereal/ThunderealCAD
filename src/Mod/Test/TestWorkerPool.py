# SPDX-License-Identifier: LGPL-2.1-or-later
"""App::WorkerPool on its own (docs/WorkerProcess.md): its lifecycle, a job
handed back exactly once, a worker that dies, a handler that fails or is
unfit, an edit of the design dropping the jobs it reaches and moving the
workers along the log to the next copy.

The kinds below are registered when this module is imported, which a
worker does before its first job of one (module="TestWorkerPool").
"""

import os
import tempfile
import time
import unittest

import FreeCAD as App
from freecad import worker

MODULE = "TestWorkerPool"


def _echo(doc, inputs, out):
    return {"Echo": inputs, "Pid": os.getpid(), "Document": doc.Name if doc else ""}


def _sleep(doc, inputs, out):
    time.sleep(inputs.get("Seconds", 0.2))
    return {"Pid": os.getpid()}


def _fail(doc, inputs, out):
    raise ValueError("asked to fail")


def _unfit(doc, inputs, out):
    raise worker.Unfit("not here")


def _dieOnce(doc, inputs, out):
    """Dies the first time, as a worker can for a reason not the job's"""
    marker = inputs["Marker"]
    if not os.path.exists(marker):
        with open(marker, "w") as f:
            f.write(str(os.getpid()))
        os._exit(3)
    return {"Pid": os.getpid()}


def _read(doc, inputs, out):
    obj = doc.getObject(inputs["Object"])
    return {"Value": getattr(obj, inputs["Property"])}


def _volume(doc, inputs, out):
    return {"Volume": doc.getObject(inputs["Object"]).Shape.Volume}


def _names(doc, inputs, out):
    return {"Names": sorted(o.Name for o in doc.Objects), "Pid": os.getpid()}


def _indexed(doc, inputs, out):
    return {"Indexed": doc.Hasher.IndexedNames}


def _table(doc, inputs, out):
    """The strings of the worker's table spelled inputs["Spelled"], by that
    spelling: None for one it lacks"""
    h = doc.Hasher
    got = {}
    for spelled in inputs["Spelled"]:
        sid = h.getIDBySpelling(spelled)
        got[str(spelled)] = sid.Data if sid else None
    return {"Size": h.Size, "Strings": got}


def _edges(doc, inputs, out):
    return {n: len(doc.getObject(n).Edges) for n in inputs["Objects"]}


def _write(doc, inputs, out):
    with open(out, "w") as f:
        f.write(inputs["Text"])
    return {}


# What each process executed of the Python features below, by name: a
# recompute a worker made leaves this process's count as it was
_executions = {}


class _Counted:
    """A Python feature: its execute() counts where it runs, says which
    process ran it, sleeps when asked, and makes an object when asked"""

    def __init__(self, obj):
        obj.addProperty("App::PropertyInteger", "Input")
        obj.addProperty("App::PropertyInteger", "Result")
        obj.addProperty("App::PropertyInteger", "MadeBy")
        obj.addProperty("App::PropertyFloat", "Sleep")
        obj.addProperty("App::PropertyBool", "MakeOne")
        obj.addProperty("App::PropertyBool", "AddOne")
        obj.Proxy = self

    def execute(self, obj):
        _executions[obj.Name] = _executions.get(obj.Name, 0) + 1
        if obj.Sleep:
            time.sleep(obj.Sleep)
        if obj.MakeOne:
            obj.Document.addObject("App::FeatureTest", "Made")
        if obj.AddOne and "Added" not in obj.PropertiesList:
            obj.addProperty("App::PropertyInteger", "Added", "Extra", "added by execute()")
        if obj.AddOne:
            obj.Added = obj.Input + 7
        obj.Result = obj.Input * 2
        obj.MadeBy = os.getpid()

    def dumps(self):
        return None

    def loads(self, state):
        return None

    def __getstate__(self):
        return None

    def __setstate__(self, state):
        return None


for _kind, _handler in (
    ("test.echo", _echo),
    ("test.sleep", _sleep),
    ("test.fail", _fail),
    ("test.unfit", _unfit),
    ("test.dieonce", _dieOnce),
    ("test.read", _read),
    ("test.write", _write),
    ("test.volume", _volume),
    ("test.edges", _edges),
    ("test.names", _names),
    ("test.indexed", _indexed),
    ("test.table", _table),
):
    worker.register(_kind, _handler)


class TestWorkerPool(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("WorkerPool")
        self.obj = self.doc.addObject("App::FeatureTest", "Feature")
        self.obj.Integer = 42
        self.doc.recompute()
        self.settings(2)
        self.pool = App.workerPool(self.doc)

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def settings(self, workers, idle=None):
        group = App.ParamGet("User parameter:BaseApp/Preferences/Workers")
        group.SetInt("Workers", workers)
        self.addCleanup(group.RemInt, "Workers")
        if idle is not None:
            group.SetFloat("IdleSeconds", idle)
            self.addCleanup(group.RemFloat, "IdleSeconds")

    def submit(self, kind, inputs=None, tag=""):
        job = self.pool.submit(kind, inputs or {}, module=MODULE, tag=tag)
        self.assertGreater(job, 0, self.pool.info()["Reason"])
        return job

    def waitFor(self, condition, seconds=60):
        end = time.time() + seconds
        while time.time() < end:
            if condition():
                return True
            self.pool.poll(50)
        return condition()

    def testJobsComeBackOnce(self):
        self.assertTrue(self.pool.start(), self.pool.info()["Reason"])
        self.assertTrue(self.pool.info()["Started"])
        jobs = [self.submit("test.echo", {"N": n}) for n in range(6)]
        self.assertTrue(self.pool.wait(120), self.pool.info())
        pids = set()
        for n, job in enumerate(jobs):
            result = self.pool.result(job)
            self.assertIsNotNone(result, job)
            self.assertEqual(result["Status"], "Done", result)
            self.assertEqual(result["Output"]["Echo"], {"N": n})
            self.assertNotEqual(result["Output"]["Pid"], os.getpid())
            self.assertTrue(result["Output"]["Document"])
            pids.add(result["Output"]["Pid"])
            self.assertIsNone(self.pool.result(job))  # handed over once
            self.assertEqual(self.pool.state(job), "Unknown")
        info = self.pool.info()
        self.assertEqual(info["Done"], 6)
        self.assertEqual(info["Failed"], 0)
        self.assertLessEqual(len(pids), 2)
        self.assertLessEqual(info["Spawned"], 2)

    def testTheCopyIsTheDocument(self):
        self.assertTrue(self.pool.start())
        job = self.submit("test.read", {"Object": "Feature", "Property": "Integer"})
        self.assertTrue(self.pool.wait(120))
        self.assertEqual(self.pool.result(job)["Output"]["Value"], 42)

    def testTheCopyIsAVersionOfTheLog(self):
        """Without the store read by the workers, the version written out
        here as an unpacked project: the fallback (sec 3)"""
        group = App.ParamGet("User parameter:BaseApp/Preferences/Workers")
        group.SetBool("ReadStore", False)
        self.addCleanup(group.RemBool, "ReadStore")
        versions = len(self.doc.getTransactionVersions())
        self.assertTrue(self.pool.start())
        info = self.pool.info()
        self.assertGreater(info["Version"], 0)
        self.assertFalse(info["ReadStore"])
        self.assertTrue(os.path.isdir(os.path.join(info["Copy"], "version")))
        self.assertEqual(len(self.doc.getTransactionVersions()), versions + 1)
        job = self.submit("test.read", {"Object": "Feature", "Property": "Integer"})
        self.assertTrue(self.pool.wait(120))
        self.assertEqual(self.pool.result(job)["Output"]["Value"], 42)

    def testTheWorkersReadTheStore(self):
        """The version is read by the workers from this process's store:
        nothing is written out here but the blobs it keeps in files, once
        each, which a second copy after an edit shares"""
        import Part  # noqa: F401

        box = self.doc.addObject("Part::Box", "Box")
        cyl = self.doc.addObject("Part::Cylinder", "Cylinder")
        cyl.Placement.Base = App.Vector(30, 0, 0)
        self.doc.recompute()
        self.assertTrue(self.pool.start(), self.pool.info()["Reason"])
        info = self.pool.info()
        self.assertGreater(info["Version"], 0)
        self.assertTrue(info["ReadStore"])
        self.assertFalse(os.path.exists(os.path.join(info["Copy"], "version")))
        first = set(os.listdir(info["Blobs"]))
        self.assertGreaterEqual(len(first), 2)  # each shape's
        job = self.submit("test.volume", {"Object": "Box"})
        self.assertTrue(self.pool.wait(120))
        self.assertAlmostEqual(self.pool.result(job)["Output"]["Volume"], 1000.0)
        box.Height = 20  # a new copy: only the box's shape is new
        self.doc.recompute()
        self.assertTrue(self.pool.info()["Started"])
        self.assertTrue(self.pool.info()["Stale"])
        self.assertTrue(self.pool.start())
        second = set(os.listdir(self.pool.info()["Blobs"]))
        self.assertTrue(first <= second)
        self.assertLessEqual(len(second - first), 2, second - first)
        jobs = [self.submit("test.volume", {"Object": n}) for n in ("Box", "Cylinder")]
        self.assertTrue(self.pool.wait(120))
        self.assertAlmostEqual(self.pool.result(jobs[0])["Output"]["Volume"], 2000.0)
        self.assertAlmostEqual(self.pool.result(jobs[1])["Output"]["Volume"], cyl.Shape.Volume)

    def testTheFilesOfAValueTheLogHolds(self):
        """A value the log holds is not saved again in its version: its
        files are its attachments in the log -- two fillets' edge lists,
        each its own (log sec 23.3)"""
        import Part  # noqa: F401

        fillets = []
        for i, count in enumerate((12, 4)):
            box = self.doc.addObject("Part::Box", "Box")
            box.Placement.Base = App.Vector(20 * i, 0, 0)
            self.doc.recompute()
            fillet = self.doc.addObject("Part::Fillet", "Fillet")
            fillet.Base = box
            fillet.Edges = [(e + 1, 1.0, 1.0) for e in range(count)]
            fillets.append(fillet.Name)
        self.doc.recompute()
        self.doc.openTransaction("Edit")
        self.doc.getObject(fillets[1]).Edges = [(e + 1, 0.5, 0.5) for e in range(3)]
        self.doc.commitTransaction()
        self.doc.recompute()
        for store in (True, False):
            group = App.ParamGet("User parameter:BaseApp/Preferences/Workers")
            group.SetBool("ReadStore", store)
            self.addCleanup(group.RemBool, "ReadStore")
            self.assertTrue(self.pool.start(), self.pool.info()["Reason"])
            job = self.submit("test.edges", {"Objects": fillets})
            self.assertTrue(self.pool.wait(120))
            result = self.pool.result(job)
            self.assertEqual(result["Status"], "Done", result)
            self.assertEqual(result["Output"], {fillets[0]: 12, fillets[1]: 3}, store)
            self.pool.stop()

    def testAFileASaveWroteGoesWithItsValue(self):
        """A save stores each value as a part of its version, and the log
        holds it from there: the file a list wrote beside it -- a fillet's
        `Fillet.Edges.bin` -- is its attachment, or a version after the
        save, which claims the value, names a file it does not carry"""
        import Part  # noqa: F401

        box = self.doc.addObject("Part::Box", "Box")
        self.doc.recompute()
        fillet = self.doc.addObject("Part::Fillet", "Fillet")
        fillet.Base = box
        fillet.Edges = [(e + 1, 1.0, 1.0) for e in range(12)]
        self.doc.recompute()
        path = os.path.join(tempfile.gettempdir(), "TestWorkerPool-%d.FCStd" % os.getpid())
        self.addCleanup(lambda: os.path.exists(path) and os.remove(path))
        self.doc.saveCopy(path)
        for store in (True, False):
            group = App.ParamGet("User parameter:BaseApp/Preferences/Workers")
            group.SetBool("ReadStore", store)
            self.addCleanup(group.RemBool, "ReadStore")
            self.assertTrue(self.pool.start(), self.pool.info()["Reason"])
            self.assertGreater(self.pool.info()["Version"], 0)
            job = self.submit("test.edges", {"Objects": ["Fillet"]})
            self.assertTrue(self.pool.wait(120))
            result = self.pool.result(job)
            self.assertEqual(result["Output"], {"Fillet": 12}, (store, result))
            self.pool.stop()

    def testTheVersionIsHeldWhileTheWorkersHoldIt(self):
        """Retention lets an unnamed version go past the number kept, but
        not one the pool's copy is: a worker started later reads it"""
        group = App.ParamGet("User parameter:BaseApp/Preferences/Document")
        group.SetInt("TransactionLogKeepVersions", 2)
        self.addCleanup(group.RemInt, "TransactionLogKeepVersions")
        self.settings(1, idle=0)
        self.assertTrue(self.pool.start())
        held = self.pool.info()["Version"]
        for _ in range(4):
            self.doc.snapshotTransactionLog()
        nums = [v["num"] for v in self.doc.getTransactionVersions()]
        self.assertIn(held, nums)
        self.assertTrue(self.waitFor(lambda: self.pool.info()["Workers"] == 0, 30))
        job = self.submit("test.read", {"Object": "Feature", "Property": "Integer"})
        self.assertTrue(self.pool.wait(120))
        self.assertEqual(self.pool.result(job)["Output"]["Value"], 42)
        self.pool.stop()
        self.doc.snapshotTransactionLog()
        nums = [v["num"] for v in self.doc.getTransactionVersions()]
        self.assertNotIn(held, nums)

    def testATransactionOpenTakesASavedCopy(self):
        self.doc.openTransaction("Open")
        self.obj.Integer = 43  # the transaction is open from its first write
        self.assertTrue(self.pool.start(), self.pool.info()["Reason"])
        self.assertEqual(self.pool.info()["Version"], 0)
        job = self.submit("test.read", {"Object": "Feature", "Property": "Integer"})
        self.assertTrue(self.pool.wait(120))
        self.assertEqual(self.pool.result(job)["Output"]["Value"], 43)
        self.doc.abortTransaction()

    def testClaimTakesBackWhatIsQueuedAndWaitsForWhatRuns(self):
        self.settings(1)
        self.assertTrue(self.pool.start())
        jobs = [self.submit("test.sleep", {"Seconds": 0.3}) for _ in range(4)]
        self.assertTrue(self.waitFor(lambda: self.pool.state(jobs[0]) == "Running"))
        self.assertEqual(self.pool.state(jobs[-1]), "Queued")
        self.assertEqual(self.pool.claim(jobs[-1]), "Yours")
        self.assertEqual(self.pool.state(jobs[-1]), "Unknown")
        self.assertEqual(self.pool.claim(jobs[0]), "Done")
        self.assertEqual(self.pool.result(jobs[0])["Status"], "Done")
        self.assertEqual(self.pool.claim(jobs[0]), "Unknown")
        self.assertTrue(self.pool.wait(120))
        self.assertIsNone(self.pool.result(jobs[-1]))  # the client's, never a worker's
        for job in jobs[1:-1]:
            self.assertEqual(self.pool.result(job)["Status"], "Done")

    def testAWorkerThatDiesHasItsJobDoneAgain(self):
        marker = os.path.join(tempfile.gettempdir(), "TestWorkerPool-%d.died" % os.getpid())
        if os.path.exists(marker):
            os.remove(marker)
        self.addCleanup(lambda: os.path.exists(marker) and os.remove(marker))
        self.assertTrue(self.pool.start())
        job = self.submit("test.dieonce", {"Marker": marker})
        self.assertTrue(self.pool.wait(120), self.pool.info())
        result = self.pool.result(job)
        self.assertEqual(result["Status"], "Done", result)
        with open(marker) as f:
            died = int(f.read())
        self.assertNotEqual(result["Output"]["Pid"], died)
        self.assertGreaterEqual(self.pool.info()["Spawned"], 2)

    def testAWorkerKilledMidJob(self):
        self.assertTrue(self.pool.start())
        job = self.submit("test.sleep", {"Seconds": 2.0})
        self.assertTrue(self.waitFor(lambda: self.pool.state(job) == "Running"))
        pids = self.pool.info()["Pids"]
        self.assertTrue(pids)
        import signal

        for pid in pids:
            os.kill(pid, signal.SIGKILL if hasattr(signal, "SIGKILL") else signal.SIGTERM)
        self.assertTrue(self.pool.wait(120), self.pool.info())
        result = self.pool.result(job)
        self.assertEqual(result["Status"], "Done", result)
        self.assertNotIn(result["Output"]["Pid"], pids)

    def testFailedAndUnfit(self):
        self.assertTrue(self.pool.start())
        failed = self.submit("test.fail")
        unfit = self.submit("test.unfit")
        unknown = self.pool.submit("test.nosuchkind", {})
        self.assertTrue(self.pool.wait(120))
        result = self.pool.result(failed)
        self.assertEqual(result["Status"], "Failed")
        self.assertIn("asked to fail", result["Error"])
        result = self.pool.result(unfit)
        self.assertEqual(result["Status"], "Unfit")
        self.assertIn("not here", result["Error"])
        self.assertEqual(self.pool.result(unknown)["Status"], "Unfit")
        # Still there for the next
        job = self.submit("test.echo", {"After": True})
        self.assertTrue(self.pool.wait(120))
        self.assertEqual(self.pool.result(job)["Status"], "Done")

    def testAFileWritten(self):
        self.assertTrue(self.pool.start())
        job = self.submit("test.write", {"Text": "made there"})
        self.assertTrue(self.pool.wait(120))
        result = self.pool.result(job)
        self.assertEqual(result["Status"], "Done")
        self.assertTrue(result["File"].startswith(self.pool.info()["Directory"]))
        with open(result["File"]) as f:
            self.assertEqual(f.read(), "made there")

    def testCancel(self):
        self.settings(1)
        self.assertTrue(self.pool.start())
        first = self.submit("test.sleep", {"Seconds": 0.3}, tag="a")
        others = [self.submit("test.echo", {}, tag="b") for _ in range(3)]
        self.assertEqual(self.pool.cancel("b"), 3)
        self.assertTrue(self.pool.wait(120))
        self.assertEqual(self.pool.result(first)["Status"], "Done")
        for job in others:
            self.assertIsNone(self.pool.result(job))
        self.assertEqual(self.pool.info()["Done"], 1)

    def testAnEditDropsTheJobsAndMovesTheWorkers(self):
        """An edit drops the jobs that say nothing of what they read; the
        workers stay, and move to the next copy along the log's rows
        (docs/WorkerProcess.md sec 4)"""
        self.settings(1)
        self.assertTrue(self.pool.start())
        pids = self.pool.info()["Pids"]
        copy = self.pool.info()["Copy"]
        self.assertTrue(os.path.isdir(copy))
        jobs = [self.submit("test.sleep", {"Seconds": 0.3}) for _ in range(3)]
        self.assertTrue(self.waitFor(lambda: self.pool.state(jobs[0]) == "Running"))
        self.doc.openTransaction("Edit")
        self.obj.Integer = 7  # the design changed
        self.doc.commitTransaction()
        info = self.pool.info()
        self.assertTrue(info["Started"])
        self.assertTrue(info["Stale"])
        self.assertEqual(info["Workers"], 1)
        for job in jobs:
            result = self.pool.result(job)
            self.assertEqual(result["Status"], "Cancelled")
            self.assertEqual(result["Error"], "the design changed")
        # The next job is made on the design as it is now, by the same
        # worker, moved there
        job = self.submit("test.read", {"Object": "Feature", "Property": "Integer"})
        self.assertFalse(self.pool.info()["Stale"])
        self.assertTrue(self.pool.wait(120))
        result = self.pool.result(job)
        self.assertEqual(result["Output"]["Value"], 7, result)
        info = self.pool.info()
        self.assertEqual(info["Pids"], pids)
        self.assertEqual(info["Spawned"], 1)
        self.assertEqual((info["Moves"], info["Reopens"]), (1, 0), info)
        self.assertNotEqual(info["Copy"], copy)
        self.assertFalse(os.path.exists(copy))  # no worker is on it

    def testAJobAnEditDoesNotReachRuns(self):
        """A job that says what it reads is dropped only by an edit of one
        of them, or of what they depend on"""
        self.settings(1)
        other = self.doc.addObject("App::FeatureTest", "Other")
        user = self.doc.addObject("App::FeatureTest", "User")
        user.setExpression("Integer", "Other.Integer + 1")
        self.doc.recompute()
        self.assertTrue(self.pool.start())
        first = self.pool.submit("test.sleep", {"Seconds": 0.3}, module=MODULE, reads=["Feature"])
        kept = self.pool.submit("test.read", {"Object": "Feature", "Property": "Integer"},
                                module=MODULE, reads=[self.obj])
        dropped = self.pool.submit("test.read", {"Object": "User", "Property": "Integer"},
                                   module=MODULE, reads=[user])
        self.assertTrue(first and kept and dropped)
        self.doc.openTransaction("Edit")
        other.Integer = 5  # what User depends on: not what Feature does
        self.doc.commitTransaction()
        self.assertEqual(self.pool.result(dropped)["Status"], "Cancelled")
        self.assertTrue(self.pool.wait(120))
        self.assertEqual(self.pool.result(first)["Status"], "Done")
        self.assertEqual(self.pool.result(kept)["Output"]["Value"], 42)
        self.assertTrue(self.pool.info()["Stale"])

    def testMovesAlongTheLog(self):
        """Shapes changed, an object added and one removed: the worker's
        copy follows the rows, and is the design"""
        import Part  # noqa: F401

        self.settings(1)
        box = self.doc.addObject("Part::Box", "Box")
        cyl = self.doc.addObject("Part::Cylinder", "Cylinder")
        cyl.Placement.Base = App.Vector(5, 5, -1)
        cut = self.doc.addObject("Part::Cut", "Cut")
        cut.Base = box
        cut.Tool = cyl
        gone = self.doc.addObject("App::FeatureTest", "Gone")
        self.doc.recompute()
        self.assertTrue(self.pool.start(), self.pool.info()["Reason"])
        job = self.submit("test.volume", {"Object": "Cut"})
        self.assertTrue(self.pool.wait(120))
        self.assertAlmostEqual(self.pool.result(job)["Output"]["Volume"], cut.Shape.Volume)
        pids = self.pool.info()["Pids"]
        for step in range(3):
            self.doc.openTransaction("Edit %d" % step)
            box.Height = 12 + step
            cyl.Radius = 2 + step * 0.5
            if step == 1:
                self.doc.addObject("App::FeatureTest", "Added")
                self.doc.removeObject(gone.Name)
            self.doc.commitTransaction()
            self.doc.recompute()
            jobs = [self.submit("test.volume", {"Object": "Cut"}), self.submit("test.names")]
            self.assertTrue(self.pool.wait(120))
            volume, names = (self.pool.result(j) for j in jobs)
            self.assertEqual(volume["Status"], "Done", volume)
            self.assertAlmostEqual(volume["Output"]["Volume"], cut.Shape.Volume, 6)
            self.assertEqual(names["Output"]["Names"], sorted(o.Name for o in self.doc.Objects))
        info = self.pool.info()
        self.assertEqual(info["Pids"], pids)
        self.assertEqual((info["Moves"], info["Reopens"]), (3, 0), info)

    def testTheCopySpellsNamesAsThisTable(self):
        """A copy opened from the log took the new-document mode of naming
        an element's index; an older file's table spells them whole, and
        every name the worker made then differed from this process's
        (docs/WorkerProcess.md sec 20)"""
        self.settings(1)
        for indexed in (not self.doc.Hasher.IndexedNames, self.doc.Hasher.IndexedNames):
            self.doc.Hasher.IndexedNames = indexed
            self.pool.stop()
            self.assertTrue(self.pool.start())
            job = self.submit("test.indexed")
            self.assertTrue(self.pool.wait(120))
            result = self.pool.result(job)
            self.assertEqual(result["Status"], "Done", result)
            self.assertEqual(result["Output"]["Indexed"], indexed)

    def testAnObjectAddedOrRemovedAlone(self):
        """No value of the design changes, and the copy is stale all the
        same"""
        self.settings(1)
        self.assertTrue(self.pool.start())
        job = self.submit("test.names")
        self.assertTrue(self.pool.wait(120))
        self.assertEqual(self.pool.result(job)["Output"]["Names"], ["Feature"])
        for step in range(2):
            self.doc.openTransaction("Step")
            if step == 0:
                self.doc.addObject("App::FeatureTest", "Alone")
            else:
                self.doc.removeObject("Alone")
            self.doc.commitTransaction()
            self.assertTrue(self.pool.info()["Stale"])
            job = self.submit("test.names")
            self.assertTrue(self.pool.wait(120))
            want = sorted(o.Name for o in self.doc.Objects)
            self.assertEqual(self.pool.result(job)["Output"]["Names"], want)
        self.assertEqual(self.pool.info()["Moves"], 2, self.pool.info())

    def testAnEditWithUndoOffMoves(self):
        """With undo off the log still records each write, in a transaction
        of its own that the next copy commits, as a save does: the workers
        move along its row"""
        self.settings(1)
        self.assertTrue(self.pool.start())
        job = self.submit("test.read", {"Object": "Feature", "Property": "Integer"})
        self.assertTrue(self.pool.wait(120))
        self.assertEqual(self.pool.result(job)["Output"]["Value"], 42)
        undo = self.doc.UndoMode
        self.doc.UndoMode = 0
        self.obj.Integer = 8
        job = self.submit("test.read", {"Object": "Feature", "Property": "Integer"})
        self.assertTrue(self.pool.wait(120))
        self.doc.UndoMode = undo
        self.assertEqual(self.pool.result(job)["Output"]["Value"], 8)
        info = self.pool.info()
        self.assertEqual((info["Moves"], info["Reopens"]), (1, 0), info)

    def testFromASavedCopyTheWorkersReopen(self):
        """A worker on a copy saved with a transaction open has no version
        to move from: it opens the next copy whole -- a fillet whose edge
        list the save wrote among them"""
        import Part  # noqa: F401

        self.settings(1)
        box = self.doc.addObject("Part::Box", "Box")
        self.doc.recompute()
        fillet = self.doc.addObject("Part::Fillet", "Fillet")
        fillet.Base = box
        fillet.Edges = [(e + 1, 1.0, 1.0) for e in range(12)]
        self.doc.recompute()
        self.doc.openTransaction("Open")
        self.obj.Integer = 43
        self.assertTrue(self.pool.start(), self.pool.info()["Reason"])
        self.assertEqual(self.pool.info()["Version"], 0)
        job = self.submit("test.edges", {"Objects": ["Fillet"]})
        self.assertTrue(self.pool.wait(120))
        self.assertEqual(self.pool.result(job)["Output"], {"Fillet": 12})
        self.doc.commitTransaction()
        self.doc.openTransaction("Edit")
        box.Height = 20
        self.doc.commitTransaction()
        self.doc.recompute()
        jobs = [self.submit("test.edges", {"Objects": ["Fillet"]}),
                self.submit("test.volume", {"Object": "Fillet"})]
        self.assertTrue(self.pool.wait(120))
        self.assertGreater(self.pool.info()["Version"], 0)
        self.assertEqual(self.pool.result(jobs[0])["Output"], {"Fillet": 12})
        self.assertAlmostEqual(self.pool.result(jobs[1])["Output"]["Volume"],
                               fillet.Shape.Volume, 6)
        info = self.pool.info()
        self.assertEqual((info["Moves"], info["Reopens"]), (0, 1), info)

    def testIdleWorkersGo(self):
        self.settings(1, idle=0.5)
        self.assertTrue(self.pool.start())
        job = self.submit("test.echo")
        self.assertTrue(self.pool.wait(120))
        self.assertEqual(self.pool.result(job)["Status"], "Done")
        self.assertTrue(self.waitFor(lambda: self.pool.info()["Workers"] == 0, 30))
        self.assertTrue(self.pool.info()["Started"])  # the copy stands
        job = self.submit("test.echo")  # a worker again, on it
        self.assertTrue(self.pool.wait(120))
        self.assertEqual(self.pool.result(job)["Status"], "Done")
        self.assertEqual(self.pool.info()["Spawned"], 2)

    def testOff(self):
        self.settings(0)
        self.assertFalse(self.pool.start())
        self.assertIn("workers are off", self.pool.info()["Reason"])
        self.assertEqual(self.pool.submit("test.echo", {}), 0)

    def testTheDocumentClosingEndsThem(self):
        self.assertTrue(self.pool.start())
        directory = self.pool.info()["Directory"]
        pids = self.pool.info()["Pids"]
        other = App.newDocument("WorkerPoolOther")
        App.closeDocument(other.Name)
        self.assertTrue(os.path.isdir(directory))
        App.closeDocument(self.doc.Name)
        self.assertFalse(os.path.exists(directory))
        for pid in pids:
            gone = False
            for _ in range(100):
                try:
                    os.kill(pid, 0)
                except OSError:
                    gone = True
                    break
                time.sleep(0.02)
            self.assertTrue(gone, pid)
        self.doc = App.newDocument("WorkerPool")  # for tearDown


class TestWorkerRecompute(unittest.TestCase):
    """A recompute made by a worker, the pool's second client
    (docs/WorkerProcess.md sec 19)"""

    def setUp(self):
        self.doc = App.newDocument("WorkerRecompute")
        group = App.ParamGet("User parameter:BaseApp/Preferences/Workers")
        group.SetInt("Workers", 1)
        self.addCleanup(group.RemInt, "Workers")
        self.pool = App.workerPool(self.doc)

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def counted(self, name, **values):
        obj = self.doc.addObject("App::FeaturePython", name)
        _Counted(obj)
        for key, value in values.items():
            setattr(obj, key, value)
        return obj

    def touched(self):
        return sorted(o.Name for o in self.doc.Objects if "Touched" in o.State)

    def byWorker(self):
        info = self.pool.recomputeInfo()
        self.assertTrue(self.pool.recompute(), self.pool.recomputeInfo()["Reason"])
        self.assertTrue(self.pool.recomputing())
        self.assertTrue(self.pool.waitRecompute(120), self.pool.recomputeInfo())
        self.assertFalse(self.pool.recomputing())
        return self.pool.recomputeInfo()

    def testAPartChainMadeByAWorker(self):
        import Part  # noqa: F401

        box = self.doc.addObject("Part::Box", "Box")
        tool = self.doc.addObject("Part::Cylinder", "Tool")
        tool.Radius = 2
        tool.Height = 30
        tool.Placement.Base = App.Vector(5, 5, -5)
        cut = self.doc.addObject("Part::Cut", "Cut")
        cut.Base = box
        cut.Tool = tool
        tool2 = self.doc.addObject("Part::Cylinder", "Tool2")
        tool2.Radius = 1
        tool2.Height = 30
        tool2.Placement.Base = App.Vector(2, 2, -5)
        cut2 = self.doc.addObject("Part::Cut", "Cut2")
        cut2.Base = cut
        cut2.Tool = tool2
        counted = self.counted("Counted")
        counted.setExpression("Input", "Box.Length")
        self.doc.recompute()
        ran = _executions.get("Counted", 0)
        box.Length = 14
        self.assertEqual(self.touched(), ["Box"])
        info = self.byWorker()
        self.assertEqual(info["Applied"], 1, info)
        self.assertEqual(info["ExecutedHere"], 0, info)
        self.assertEqual(info["Unused"], 0, info)
        # Box, Cut, Cut2 and Counted, the tools untouched
        self.assertEqual(info["Supplied"], 4, info)
        self.assertTrue(info["ById"], info)
        self.assertGreater(info["WorkerSeconds"], 0)
        self.assertEqual(self.touched(), [])
        self.assertTrue(all(o.isValid() for o in self.doc.Objects))
        # Made there, not here, and its value here
        self.assertEqual(_executions.get("Counted", 0), ran)
        self.assertNotEqual(counted.MadeBy, os.getpid())
        self.assertEqual(counted.Result, 28)
        self.assertEqual(self.doc.recompute(), 0)  # nothing left to do
        made = (cut2.Shape.Volume, len(cut2.Shape.Faces), sorted(cut2.Shape.ElementMap.items()))
        self.assertAlmostEqual(made[0], 14 * 10 * 10 - 3.141592653589793 * (4 + 1) * 10, 6)
        # The same made here
        box.Length = 15
        self.doc.recompute()
        box.Length = 14
        self.doc.recompute()
        self.assertEqual(counted.MadeBy, os.getpid())
        here = (cut2.Shape.Volume, len(cut2.Shape.Faces), sorted(cut2.Shape.ElementMap.items()))
        self.assertAlmostEqual(made[0], here[0], 9)
        self.assertEqual(made[1], here[1])
        self.assertEqual(made[2], here[2])

    def testAnErrorComesWithItsMessage(self):
        feature = self.doc.addObject("App::FeatureTest", "Failing")
        after = self.doc.addObject("App::FeatureTest", "After")
        after.Link = feature
        self.doc.recompute()
        count = feature.ExecCount
        afterCount = after.ExecCount
        feature.ExceptionType = 2
        info = self.byWorker()
        self.assertEqual(info["Applied"], 1, info)
        self.assertFalse(feature.isValid())
        self.assertIn("Invalid", feature.State)
        message = feature.getStatusString()
        self.assertIn("Testexception", message)
        self.assertEqual(feature.ExecCount, count)
        # Not reached past the error, there nor here
        self.assertEqual(after.ExecCount, afterCount)
        states = (feature.State, after.State)
        # As a recompute here leaves it
        feature.touch()
        self.doc.recompute()
        self.assertFalse(feature.isValid())
        self.assertEqual(feature.getStatusString(), message)
        self.assertEqual(after.ExecCount, afterCount)
        self.assertEqual((feature.State, after.State), states)

    def testAMutatedBinderLeavesTheTableWhole(self):
        """A binder that copies what it binds (BindCopyOnChange 2) exports
        it, and an export numbers the string tables afresh: the document's
        came out new, index 0, its strings to go with the value. The worker
        captured the cut after that, and the put-in read the strings it
        carried -- those marked in the worker -- into this document's table,
        which its restore clears first: scanner.FCStd's table went from
        51,683 strings to 34, and every name spelled ids it did not hold
        (docs/StringTableContentIds.md sec 14)"""
        import re

        import Part  # noqa: F401

        box = self.doc.addObject("Part::Box", "Box")
        tool = self.doc.addObject("Part::Cylinder", "Tool")
        tool.Radius = 2
        tool.Height = 30
        tool.Placement.Base = App.Vector(5, 5, -5)
        cut = self.doc.addObject("Part::Cut", "Cut")
        cut.Base = box
        cut.Tool = tool
        binder = self.doc.addObject("Part::SubShapeBinder", "Binder")
        binder.Support = [(cut, "")]
        binder.BindCopyOnChange = 2
        self.doc.recompute()
        size = self.doc.Hasher.Size
        box.Length = 14
        info = self.byWorker()
        self.assertEqual(info["Applied"], 1, info)
        self.assertTrue(info["ById"], info)
        self.assertGreaterEqual(self.doc.Hasher.Size, size)
        for obj in (cut, binder):
            spelled = {int(x, 16) for k in obj.Shape.ElementMap for x in re.findall(r"#([0-9a-f]+)", k)}
            self.assertTrue(spelled, obj.Name)
            missing = [s for s in spelled if not self.doc.Hasher.getIDBySpelling(s)]
            self.assertEqual(missing, [], obj.Name)

    def testAnEditDropsIt(self):
        counted = self.counted("Slow", Sleep=1.5)
        self.doc.recompute()
        ran = _executions["Slow"]
        counted.Input = 3
        self.assertTrue(self.pool.recompute(), self.pool.recomputeInfo()["Reason"])
        counted.Input = 4  # what the job reads
        info = self.pool.recomputeInfo()
        self.assertFalse(info["Running"], info)
        self.assertEqual(info["Cancelled"], 1, info)
        self.assertIn("the design changed", info["Reason"])
        self.assertTrue(self.pool.waitRecompute(0))
        self.assertEqual(self.touched(), ["Slow"])
        self.doc.recompute()
        self.assertEqual(_executions["Slow"], ran + 1)
        self.assertEqual(counted.Result, 8)
        self.assertEqual(self.pool.recomputeInfo()["Applied"], 0)

    def testARecomputeHereTakesTheWorkers(self):
        counted = self.counted("Taken", Sleep=1.0)
        self.doc.recompute()
        ran = _executions["Taken"]
        counted.Input = 5
        self.assertTrue(self.pool.recompute(), self.pool.recomputeInfo()["Reason"])
        # Being made: the worker has it
        end = time.time() + 60
        while time.time() < end and self.pool.info()["Busy"] == 0:
            self.pool.poll(20)
        self.assertEqual(self.pool.info()["Busy"], 1, self.pool.info())
        self.doc.recompute()  # waits for the worker and takes its result
        self.assertEqual(_executions["Taken"], ran)
        self.assertEqual(counted.Result, 10)
        self.assertNotEqual(counted.MadeBy, os.getpid())
        info = self.pool.recomputeInfo()
        self.assertEqual(info["Borrowed"], 1, info)
        self.assertEqual(info["Supplied"], 1, info)
        self.assertFalse(info["Running"], info)
        self.assertEqual(self.touched(), [])
        self.pool.waitRecompute(5)
        self.assertEqual(self.pool.recomputeInfo()["Applied"], 0)

    def testOneThatMakesAnObjectIsUnfit(self):
        counted = self.counted("Maker")
        self.doc.recompute()
        counted.MakeOne = True
        self.assertTrue(self.pool.recompute(), self.pool.recomputeInfo()["Reason"])
        self.assertTrue(self.pool.waitRecompute(120))
        info = self.pool.recomputeInfo()
        self.assertEqual(info["Unfit"], 1, info)
        self.assertIn("made or removed", info["Reason"])
        self.assertEqual(self.touched(), ["Maker"])
        self.assertIsNone(self.doc.getObject("Made"))
        self.doc.recompute()  # made here
        self.assertIsNotNone(self.doc.getObject("Made"))

    def testAPropertyTheRecomputeAddsIsAddedHere(self):
        counted = self.counted("Adder")
        self.doc.recompute()
        counted.AddOne = True
        counted.Input = 2
        info = self.byWorker()
        self.assertEqual(info["Applied"], 1, info)
        self.assertEqual(info["ExecutedHere"], 0, info)
        self.assertNotEqual(counted.MadeBy, os.getpid())
        self.assertIn("Added", counted.PropertiesList)
        self.assertEqual(counted.Added, 9)
        self.assertEqual(counted.getGroupOfProperty("Added"), "Extra")
        self.assertEqual(counted.getDocumentationOfProperty("Added"), "added by execute()")
        self.assertEqual(self.touched(), [])

    def testNothingToRecompute(self):
        self.counted("Idle")
        self.doc.recompute()
        self.assertFalse(self.pool.recompute())
        self.assertIn("nothing to recompute", self.pool.recomputeInfo()["Reason"])
