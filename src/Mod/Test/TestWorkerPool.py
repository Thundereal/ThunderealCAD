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
import threading
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


def _crash():
    """Reads memory at 0: a crash in native code, as one in a kernel would be.
    On Windows ctypes runs a call under its own exception handler and raises
    instead; faulthandler's read is not guarded"""
    if os.name == "nt":
        import faulthandler

        faulthandler._read_null()
    import ctypes

    ctypes.string_at(0)


def _alive(pid):
    """Whether process `pid` still runs. On Windows os.kill(pid, 0) is not a
    probe: signal 0 there is CTRL_C_EVENT, sent to the console"""
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes

        kernel = ctypes.windll.kernel32
        handle = kernel.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION
        if not handle:
            return False
        try:
            code = wintypes.DWORD()
            return bool(kernel.GetExitCodeProcess(handle, ctypes.byref(code))) and (
                code.value == 259  # STILL_ACTIVE
            )
        finally:
            kernel.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def _fault(doc, inputs, out):
    _crash()


def _read(doc, inputs, out):
    obj = doc.getObject(inputs["Object"])
    return {"Value": getattr(obj, inputs["Property"])}


def _sleepRead(doc, inputs, out):
    time.sleep(inputs.get("Seconds", 0.2))
    return _read(doc, inputs, out)


def _say(doc, inputs, out):
    """Says through the Console what it is given, at the levels given, and
    from another thread if asked"""
    for level, text in inputs.get("Say", []):
        getattr(App.Console, "Print" + level)(text + "\n")
    if inputs.get("Thread"):
        thread = threading.Thread(target=App.Console.PrintWarning, args=(inputs["Thread"] + "\n",))
        thread.start()
        thread.join()
    if inputs.get("Unfit"):
        raise worker.Unfit("not here")
    time.sleep(inputs.get("Seconds", 0))
    return {}


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


class _Polygon:
    """A Part Python feature: a face of Sides sides"""

    def __init__(self, obj):
        obj.addProperty("App::PropertyInteger", "Sides")
        obj.addProperty("App::PropertyInteger", "MadeBy")
        obj.Sides = 4
        obj.Proxy = self

    def execute(self, obj):
        import math

        import Part

        n = obj.Sides
        points = [App.Vector(10 * math.cos(2 * math.pi * i / n),
                             10 * math.sin(2 * math.pi * i / n), 0) for i in range(n)]
        obj.Shape = Part.Face(Part.makePolygon(points + points[:1]))
        obj.MadeBy = os.getpid()

    def dumps(self):
        return None

    def loads(self, state):
        return None

    def __getstate__(self):
        return None

    def __setstate__(self, state):
        return None


class _Crashing(_Counted):
    """A _Counted that takes its process down when its Crash is set and it
    runs in any process but the one it was made in (Home): a feature whose
    execute() crashes. Run in that process, it raises instead, which the
    tests take for its having been run there"""

    def __init__(self, obj):
        super().__init__(obj)
        obj.addProperty("App::PropertyBool", "Crash")
        obj.addProperty("App::PropertyInteger", "Home")
        obj.Home = os.getpid()
        obj.Crash = True

    def execute(self, obj):
        if obj.Crash:
            if os.getpid() == obj.Home:
                _executions["crashed here"] = _executions.get("crashed here", 0) + 1
                raise RuntimeError("a crashing execute() ran in the test's own process")
            if os.name == "nt":
                _crash()  # a signal there is TerminateProcess, not a crash
            import signal

            os.kill(os.getpid(), signal.SIGSEGV)
        super().execute(obj)


for _kind, _handler in (
    ("test.echo", _echo),
    ("test.sleep", _sleep),
    ("test.fail", _fail),
    ("test.unfit", _unfit),
    ("test.dieonce", _dieOnce),
    ("test.fault", _fault),
    ("test.read", _read),
    ("test.sleepread", _sleepRead),
    ("test.say", _say),
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

    def testAKilledWorkerLeavesNothingInTheCache(self):
        """A worker's copy of the document and its reader of this process's
        store each have a transient directory, named after its pid. Ended
        by the pool -- stopped, or its document closed -- it is killed, and
        removes neither: the pool does once it is gone (docs/WorkerProcess.md
        sec 48)"""
        cache = App.getUserCachePath()

        def left(pids):
            return sorted(
                n for n in os.listdir(cache) for pid in pids
                if n.endswith("_%d" % pid) and ("_Doc_" in n or "_Reader_" in n))

        self.assertTrue(self.pool.start(), self.pool.info()["Reason"])
        job = self.submit("test.read", {"Object": "Feature", "Property": "Integer"})
        self.assertTrue(self.pool.wait(120), self.pool.info())
        self.assertEqual(self.pool.result(job)["Output"]["Value"], 42)
        self.assertTrue(self.pool.info()["ReadStore"])
        pids = list(self.pool.info()["Pids"])
        self.assertTrue(pids)
        # what they made: the copy's directory and the reader's
        self.assertTrue(left(pids))
        self.pool.stop("the test is over")
        self.assertEqual(left(pids), [])
        # and by a close of the document
        self.assertTrue(self.pool.start(), self.pool.info()["Reason"])
        job = self.submit("test.read", {"Object": "Feature", "Property": "Integer"})
        self.assertTrue(self.pool.wait(120), self.pool.info())
        pids = list(self.pool.info()["Pids"])
        self.assertTrue(left(pids))
        App.closeDocument(self.doc.Name)
        self.doc = App.newDocument("WorkerPool")
        self.assertEqual(left(pids), [])

    def testAFirstSaveEndsTheWorkersReadingTheStore(self):
        """A first save renames the transient directory, which Windows will
        not do while a worker has the store in it open: the workers reading
        it are ended first, and the next job's copy is read from where the
        store went (docs/WorkerProcess.md sec 45)"""
        import shutil

        self.assertTrue(self.pool.start(), self.pool.info()["Reason"])
        job = self.submit("test.read", {"Object": "Feature", "Property": "Integer"})
        self.assertTrue(self.pool.wait(120), self.pool.info())
        self.assertEqual(self.pool.result(job)["Output"]["Value"], 42)
        info = self.pool.info()
        self.assertTrue(info["ReadStore"], info)
        before = set(info["Pids"])
        self.assertTrue(before, info)
        was = self.doc.TransientDir
        folder = tempfile.mkdtemp(prefix="fc-firstsave-")
        self.addCleanup(shutil.rmtree, folder, True)
        self.doc.saveAs(os.path.join(folder, "first.FCStd"))
        self.assertNotEqual(self.doc.TransientDir, was)
        self.assertFalse(os.path.exists(was))
        info = self.pool.info()
        self.assertFalse(before & set(info["Pids"]), info)
        self.assertTrue(info["Stale"], info)
        self.obj.Integer = 43
        self.doc.recompute()
        job = self.submit("test.read", {"Object": "Feature", "Property": "Integer"})
        self.assertTrue(self.pool.wait(120), self.pool.info())
        result = self.pool.result(job)
        self.assertEqual(result["Status"], "Done", result)
        self.assertEqual(result["Output"]["Value"], 43)
        self.assertTrue(self.pool.info()["ReadStore"])

    def testTheCopyIsAVersionNothingChangedSince(self):
        """The newest version of the branch that nothing since changed the
        document from -- a save's here, an open's mostly -- is the copy:
        no snapshot is taken for it. After an edit, one is
        (docs/WorkerProcess.md sec 21)"""
        import shutil

        folder = tempfile.mkdtemp(prefix="fc-reuse-")
        self.addCleanup(shutil.rmtree, folder, True)
        self.doc.saveAs(os.path.join(folder, "reuse.FCStd"))
        versions = len(self.doc.getTransactionVersions())
        self.assertGreater(versions, 0)
        self.assertTrue(self.pool.start())
        self.assertEqual(len(self.doc.getTransactionVersions()), versions)
        job = self.submit("test.read", {"Object": "Feature", "Property": "Integer"})
        self.assertTrue(self.pool.wait(120))
        self.assertEqual(self.pool.result(job)["Output"]["Value"], 42)
        self.pool.stop()

        self.obj.Integer = 43
        self.assertTrue(self.pool.start())
        self.assertEqual(len(self.doc.getTransactionVersions()), versions + 1)
        job = self.submit("test.read", {"Object": "Feature", "Property": "Integer"})
        self.assertTrue(self.pool.wait(120))
        self.assertEqual(self.pool.result(job)["Output"]["Value"], 43)

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

    def readyWorker(self):
        """A worker standing, its start measured: the pid"""
        self.settings(1)
        self.assertTrue(self.pool.start())
        job = self.submit("test.echo")
        self.assertTrue(self.pool.wait(120))
        self.assertEqual(self.pool.result(job)["Status"], "Done")
        self.assertGreater(self.pool.info()["StartSeconds"], 0)
        return self.pool.info()["Pids"][0]

    def longJobRunning(self, kind="test.sleep", **submit):
        """A job of a minute, run past EndAfter"""
        inputs = {"Seconds": 60.0, "Object": "Feature", "Property": "Integer"}
        job = self.pool.submit(kind, inputs, module=MODULE, **submit)
        self.assertGreater(job, 0)
        self.assertTrue(self.waitFor(lambda: self.pool.state(job) == "Running"))
        after = self.pool.info()["EndAfter"]
        self.assertGreaterEqual(after, self.pool.info()["StartSeconds"])
        began = time.time()
        self.waitFor(lambda: time.time() - began > after + 0.2, after + 10)
        return job

    def gone(self, pid):
        return not _alive(pid)

    def testACancelledLongJobEndsItsWorker(self):
        """A running job cancelled after it ran past EndAfter -- longer than a
        worker takes to start, and 2 s -- ends its worker (docs/WorkerProcess.md sec 26): the minute it would
        go on making is not spent, and the next job has a new worker"""
        pid = self.readyWorker()
        job = self.longJobRunning(tag="long")
        began = time.time()
        self.assertEqual(self.pool.cancel("long"), 1)
        info = self.pool.info()
        self.assertEqual(info["Ended"], 1, info)
        self.assertEqual((info["Busy"], info["Failed"]), (0, 0), info)
        self.assertNotIn(pid, info["Pids"])
        self.assertTrue(self.gone(pid))
        self.assertEqual(self.pool.state(job), "Unknown")
        after = self.submit("test.echo")
        self.assertTrue(self.pool.wait(120), self.pool.info())
        result = self.pool.result(after)
        self.assertEqual(result["Status"], "Done", result)
        self.assertNotEqual(result["Output"]["Pid"], pid)
        self.assertLess(time.time() - began, 30)
        self.assertIsNone(self.pool.result(job))  # cancelled: no one is told
        self.assertTrue(self.pool.info()["Started"])

    def testAJobCancelledSoonLetsItsWorkerFinish(self):
        """Cancelled before it ran past EndAfter: the worker finishes it, and
        is there for the next"""
        pid = self.readyWorker()
        job = self.submit("test.sleep", {"Seconds": 1.0}, tag="short")
        self.assertTrue(self.waitFor(lambda: self.pool.state(job) == "Running"))
        self.assertEqual(self.pool.cancel("short"), 1)
        self.assertEqual(self.pool.info()["Ended"], 0)
        self.assertEqual(self.pool.info()["Busy"], 1)
        after = self.submit("test.echo")
        self.assertTrue(self.pool.wait(120))
        self.assertEqual(self.pool.result(after)["Output"]["Pid"], pid)
        self.assertEqual(self.pool.info()["Spawned"], 1)

    def testALongJobAnEditDropsEndsItsWorker(self):
        """The same for a job an edit of what it reads drops: handed back
        Cancelled, its worker ended"""
        pid = self.readyWorker()
        job = self.longJobRunning("test.sleepread", reads=["Feature"])
        self.doc.openTransaction("Edit")
        self.obj.Integer = 7
        self.doc.commitTransaction()
        result = self.pool.result(job)
        self.assertEqual(result["Status"], "Cancelled", result)
        self.assertFalse(result["Died"])
        info = self.pool.info()
        self.assertEqual(info["Ended"], 1, info)
        self.assertNotIn(pid, info["Pids"])
        after = self.submit("test.read", {"Object": "Feature", "Property": "Integer"})
        self.assertTrue(self.pool.wait(120), self.pool.info())
        self.assertEqual(self.pool.result(after)["Output"]["Value"], 7)

    def testEndDroppedOffLetsTheWorkerFinish(self):
        group = App.ParamGet("User parameter:BaseApp/Preferences/Workers")
        group.SetBool("EndDropped", False)
        self.addCleanup(group.RemBool, "EndDropped")
        pid = self.readyWorker()
        self.longJobRunning(tag="t")
        self.assertEqual(self.pool.cancel("t"), 1)
        info = self.pool.info()
        self.assertEqual((info["Ended"], info["Busy"], info["Pids"]), (0, 1, [pid]), info)
        self.pool.stop()

    def listen(self, everyone=False):
        """What this process's Console says from now: (notifier, text,
        level), a worker's only unless `everyone`"""
        heard = []

        def observer(notifier, message, level):
            if everyone or notifier.startswith("worker ") or "(worker " in notifier:
                heard.append((notifier, message, level))

        App.Console.AttachObserver(observer)
        self.addCleanup(App.Console.DetachObserver, observer)
        return heard

    def testWhatAWorkerSaysIsSaidHereWithItsResult(self):
        """What a job's worker says through its Console comes with the
        result, and is said here, the worker its notifier
        (docs/WorkerProcess.md sec 27): a log line only if asked for, what
        another thread said before the result"""
        heard = self.listen()
        self.assertTrue(self.pool.start())
        job = self.submit("test.say", {"Say": [["Warning", "careful"], ["Message", "made"],
                                               ["Log", "in detail"], ["Error", "wrong"]],
                                       "Thread": "from a thread"})
        self.assertTrue(self.pool.wait(120))
        result = self.pool.result(job)
        self.assertEqual(result["Status"], "Done", result)
        said = [(m["Level"], m["Text"]) for m in result["Messages"]]
        self.assertEqual(said, [("Warning", "careful\n"), ("Message", "made\n"),
                                ("Error", "wrong\n"), ("Warning", "from a thread\n")])
        name = "worker %d" % result["Messages"][0]["Worker"]
        self.assertEqual(heard, [(name, text, level) for level, text in said])

    def testALogLineIsSaidWhenAskedFor(self):
        group = App.ParamGet("User parameter:BaseApp/Preferences/Workers")
        group.SetBool("RelayLog", True)
        self.addCleanup(group.RemBool, "RelayLog")
        heard = self.listen()
        self.assertTrue(self.pool.start())
        job = self.submit("test.say", {"Say": [["Log", "in detail"]]})
        self.assertTrue(self.pool.wait(120))
        said = [(m["Level"], m["Text"]) for m in self.pool.result(job)["Messages"]]
        self.assertEqual(said, [("Log", "in detail\n")])
        # With what it logged opening its copy, said at once
        self.assertIn(("in detail\n", "Log"), [h[1:] for h in heard])

    def testAnUnfitJobsWordsAreNotSaidHere(self):
        """Unfit, the job is done here, which says its own"""
        heard = self.listen()
        self.assertTrue(self.pool.start())
        job = self.submit("test.say", {"Say": [["Warning", "careful"]], "Unfit": True})
        self.assertTrue(self.pool.wait(120))
        result = self.pool.result(job)
        self.assertEqual(result["Status"], "Unfit")
        self.assertEqual([m["Text"] for m in result["Messages"]], ["careful\n"])
        self.assertEqual(heard, [])

    def testADroppedJobsWordsAreNotSaid(self):
        heard = self.listen()
        self.settings(1)
        self.assertTrue(self.pool.start())
        job = self.submit("test.say", {"Say": [["Warning", "careful"]], "Seconds": 0.5},
                          tag="t")
        self.assertTrue(self.waitFor(lambda: self.pool.state(job) == "Running"))
        self.pool.poll(200)  # what it said came
        self.assertEqual(self.pool.cancel("t"), 1)
        self.assertTrue(self.pool.wait(120))
        self.assertIsNone(self.pool.result(job))
        self.assertEqual(self.pool.info()["Ended"], 0)
        self.assertEqual(heard, [])

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

    def testAPinnedJobIsMadeOnItsCopy(self):
        """A pinned job is made on the copy standing when it was submitted
        (docs/WorkerProcess.md sec 24): an edit does not drop it, and the
        copy it runs on goes only after it is done"""
        self.settings(1)
        self.assertTrue(self.pool.start())
        copy = self.pool.info()["Copy"]
        pinned = self.pool.submit("test.sleepread",
                                  {"Seconds": 1.0, "Object": "Feature", "Property": "Integer"},
                                  module=MODULE, pinned=True)
        self.assertGreater(pinned, 0)
        self.assertTrue(self.waitFor(lambda: self.pool.state(pinned) == "Running"))
        self.doc.openTransaction("Edit")
        self.obj.Integer = 7
        self.doc.commitTransaction()
        self.assertEqual(self.pool.state(pinned), "Running")
        # The next job takes a newer copy; the worker moves to it after
        after = self.submit("test.read", {"Object": "Feature", "Property": "Integer"})
        self.assertNotEqual(self.pool.info()["Copy"], copy)
        self.assertTrue(os.path.isdir(copy))  # the pinned job is made there
        self.assertTrue(self.pool.wait(120))
        result = self.pool.result(pinned)
        self.assertEqual(result["Status"], "Done", result)
        self.assertEqual(result["Output"]["Value"], 42)
        self.assertEqual(self.pool.result(after)["Output"]["Value"], 7)
        self.assertFalse(os.path.exists(copy))
        self.assertEqual(self.pool.info()["Spawned"], 1)

    def testAPinnedJobNoWorkerTookIsCancelledByANewerCopy(self):
        """Queued behind another when the design moved on: no worker is left
        on its copy, and it comes back Cancelled"""
        self.settings(1)
        self.assertTrue(self.pool.start())
        busy = self.submit("test.sleep", {"Seconds": 0.5})
        pinned = self.pool.submit("test.read", {"Object": "Feature", "Property": "Integer"},
                                  module=MODULE, pinned=True)
        self.assertTrue(self.waitFor(lambda: self.pool.state(busy) == "Running"))
        self.assertEqual(self.pool.state(pinned), "Queued")
        self.doc.openTransaction("Edit")
        self.obj.Integer = 7
        self.doc.commitTransaction()
        self.assertEqual(self.pool.result(busy)["Status"], "Cancelled")
        self.assertEqual(self.pool.state(pinned), "Queued")  # an edit alone keeps it
        after = self.submit("test.read", {"Object": "Feature", "Property": "Integer"})
        result = self.pool.result(pinned)
        self.assertEqual(result["Status"], "Cancelled", result)
        self.assertIn("before a worker", result["Error"])
        self.assertTrue(self.pool.wait(120))
        self.assertEqual(self.pool.result(after)["Output"]["Value"], 7)

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
                if not _alive(pid):
                    gone = True
                    break
                time.sleep(0.02)
            self.assertTrue(gone, pid)
        self.doc = App.newDocument("WorkerPool")  # for tearDown


class TestWorkerRecompute(unittest.TestCase):
    """A recompute made by a worker, the pool's second client
    (docs/WorkerProcess.md sec 19)"""

    def setUp(self):
        # Expressions evaluated natively, here and in the workers (which take
        # this setting with the copy of the configuration their pool writes):
        # with a Pyodide runtime installed they are routed to the sandbox,
        # and a worker's first routed one boots its guest, 1.7 s on the mini
        # -- what the splits' timings would then measure (WorkerProcess.md
        # sec 66)
        sandbox = App.ParamGet("User parameter:BaseApp/Preferences/Expression/Sandbox")
        if "Evaluate" in sandbox.GetBools():
            self.addCleanup(sandbox.SetBool, "Evaluate", sandbox.GetBool("Evaluate"))
        else:
            self.addCleanup(sandbox.RemBool, "Evaluate")
        sandbox.SetBool("Evaluate", False)
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

    def splitChains(self):
        """Base read by two slow branches, Left (2.0 s) and Right (1.5 s, a
        _Crashing not set to crash),
        each read by a tail: made once here, its times known; two workers
        allowed (this class allows one)"""
        group = App.ParamGet("User parameter:BaseApp/Preferences/Workers")
        group.SetInt("Workers", 2)
        # The default (2 s) is past what these save (1.5 s): the tests' own
        group.SetFloat("SplitSeconds", 1.0)
        self.addCleanup(group.RemFloat, "SplitSeconds")
        self.pool.expectedRecompute()  # notes what each takes from now
        base = self.counted("Base", Input=1)
        left = self.counted("Left", Sleep=2.0)
        left.setExpression("Input", "Base.Result")
        right = self.doc.addObject("App::FeaturePython", "Right")
        _Crashing(right)
        right.Crash = False
        right.Sleep = 1.5
        right.setExpression("Input", "Base.Result")
        tails = []
        for name, reads in (("LeftTail", "Left"), ("RightTail", "Right")):
            tail = self.counted(name)
            tail.setExpression("Input", reads + ".Result")
            tails.append(tail)
        warm = self.counted("Warm")
        self.doc.recompute()
        # A worker that has made a job: the pool grows by one that has
        warm.Input = 1
        self.byWorker()
        return base, left, right, tails

    def testARecomputeIsSplitOverTwoWorkers(self):
        """Two branches that take long, under what both read: split over two
        workers, each half made at once by its own, put in as one recompute
        -- what both read made by both, taken from the first
        (docs/WorkerProcess.md sec 42)"""
        base, left, right, tails = self.splitChains()
        base.Input = 2
        began = time.time()
        info = self.byWorker()
        took = time.time() - began
        self.assertEqual(info["Split"], 1, info)
        self.assertGreaterEqual(info["SplitSaving"], 1.4, info)
        self.assertEqual(info["Applied"], 2, info)
        self.assertEqual(info["ExecutedHere"], 0, info)
        self.assertGreater(info["SuppliedSecond"], 0, info)
        self.assertEqual(self.touched(), [])
        self.assertEqual((left.Result, right.Result), (8, 8))
        self.assertEqual([t.Result for t in tails], [16, 16])
        # Each slow branch by a worker of its own, neither here
        self.assertNotIn(os.getpid(), (left.MadeBy, right.MadeBy))
        self.assertNotEqual(left.MadeBy, right.MadeBy)
        self.assertEqual(base.MadeBy, left.MadeBy)  # the first half's
        # Side by side: the worker's time the longer half's, and the whole
        # less than both one after the other
        self.assertLess(info["WorkerSeconds"], 2.0 + 1.0, info)
        self.assertLess(took, 2.0 + 1.5, info)

    def testACrashInOneHalfIsPutDownToItsObject(self):
        """The worker of the second half dies in Right, twice: the first half
        is put in, Right is left in its error and not run here, what reads
        it is not made from it (sec 34, 42)"""
        base, left, right, tails = self.splitChains()
        right.Crash = True
        base.Input = 2
        _executions.pop("crashed here", None)
        info = self.byWorker()
        self.assertEqual(info["Split"], 1, info)
        self.assertEqual(info["Crashed"], ["Right"], info)
        self.assertEqual(info["Applied"], 2, info)
        self.assertEqual((left.Result, tails[0].Result), (8, 16))
        self.assertNotEqual(left.MadeBy, os.getpid())
        self.assertNotIn("crashed here", _executions)
        self.assertIn("Invalid", right.State)
        self.assertNotEqual(tails[1].Result, 16)

    def testAnEditOfASpreadsheetIsMadeFromByAWorker(self):
        """A spreadsheet's values are not saved, so a worker's result has
        none: the worker makes the sheet for what reads it, and this process
        makes it again as the result is put in. Before, what read it was
        made from the value before the edit, one edit behind
        (docs/WorkerProcess.md sec 44)"""
        sheet = self.doc.addObject("Spreadsheet::Sheet", "Spreadsheet")
        sheet.set("C9", "=5.5 mm")
        sheet.set("B9", "=.C9")
        sheet.setAlias("B9", "radius")
        reader = self.counted("Reader")
        reader.setExpression("Input", "Spreadsheet.radius * 10 / 1 mm")
        self.doc.recompute()
        self.assertEqual(reader.Result, 110)
        for value in (5.6, 5.7):
            sheet.set("C9", "=%g mm" % value)
            info = self.byWorker()
            self.assertEqual(info["ExecutedHere"], 0, info)
            self.assertEqual(sheet.get("radius").Value, value)
            self.assertEqual(reader.Result, 2 * round(value * 10), value)
            self.assertNotEqual(reader.MadeBy, os.getpid())
            self.assertEqual(self.touched(), [])
        # and after a recompute here, the next edit a worker's again
        sheet.set("C9", "=5.8 mm")
        self.doc.recompute()
        sheet.set("C9", "=5.9 mm")
        self.byWorker()
        self.assertEqual(reader.Result, 118)

    def testALinkGroupsShapeFollowsAWorkersResult(self):
        """A link group's shape is made of its members' and kept: a worker's
        result put in makes the group again, so the shape kept goes. Before,
        the group kept the shape of the edit before (docs/WorkerProcess.md
        sec 44)"""
        import Part

        box = self.doc.addObject("Part::Box", "Box")
        group = self.doc.addObject("App::LinkGroup", "Group")
        group.ElementList = [box]
        self.doc.recompute()
        self.assertAlmostEqual(Part.getShape(group).Volume, 1000.0)
        for length in (20.0, 30.0):
            box.Length = length
            self.byWorker()
            self.assertAlmostEqual(box.Shape.Volume, 100.0 * length)
            self.assertAlmostEqual(Part.getShape(group).Volume, 100.0 * length, msg=length)

    def testARecomputeIsSplitOverThreeWorkers(self):
        """Three branches of 2 s each under what all read, three workers
        allowed: three parts, each branch by a worker of its own, put in as
        one recompute (docs/WorkerProcess.md sec 44). The first such
        recompute grows the pool; the second is timed"""
        group = App.ParamGet("User parameter:BaseApp/Preferences/Workers")
        group.SetInt("Workers", 3)
        group.SetFloat("SplitSeconds", 1.0)
        self.addCleanup(group.RemFloat, "SplitSeconds")
        self.pool.expectedRecompute()
        base = self.counted("Base", Input=1)
        branches = []
        for name in ("Left", "Middle", "Right"):
            branch = self.counted(name, Sleep=2.0)
            branch.setExpression("Input", "Base.Result")
            branches.append(branch)
        warm = self.counted("Warm")
        self.doc.recompute()
        warm.Input = 1
        self.byWorker()
        base.Input = 2
        info = self.byWorker()
        self.assertEqual(info["Split"], 1, info)
        self.assertEqual(len(info["SplitObjects"]), 3, info)
        base.Input = 3
        began = time.time()
        info = self.byWorker()
        took = time.time() - began
        self.assertEqual(info["Split"], 2, info)
        self.assertEqual(len(info["SplitObjects"]), 3, info)
        self.assertGreaterEqual(info["SplitSaving"], 3.5, info)
        self.assertEqual(info["ExecutedHere"], 0, info)
        self.assertEqual(self.touched(), [])
        self.assertEqual([b.Result for b in branches], [12, 12, 12])
        made = [b.MadeBy for b in branches]
        self.assertNotIn(os.getpid(), made)
        self.assertEqual(len(set(made)), 3, made)
        # Side by side: less than two of them one after the other
        self.assertLess(took, 2 * 2.0, info)

    def testARecomputeThatSavesLittleIsNotSplit(self):
        """Below Workers/SplitSeconds -- its default, 2 s, or one set -- or
        with it 0: one worker"""
        base, left, right, tails = self.splitChains()
        group = App.ParamGet("User parameter:BaseApp/Preferences/Workers")
        for value in (None, 3.0, 0.0):
            if value is None:
                group.RemFloat("SplitSeconds")  # the default: 2 s
            else:
                group.SetFloat("SplitSeconds", value)
            base.Input += 1
            info = self.byWorker()
            self.assertEqual(info["Split"], 0, (value, info))
            self.assertEqual(left.MadeBy, right.MadeBy, value)
            self.assertEqual((left.Result, right.Result), (4 * base.Input,) * 2, value)

    def testACheapJoinIsMadeHereSoWhatItGathersSplits(self):
        """An assembly's kind of object -- cheap, reading both slow branches --
        put the whole recompute in one part: the part holding it holds all
        it reads. Left out of the parts with the cheap objects before it
        that only it reads, and made here as the parts are put in, the two
        branches split; with Workers/JoinSeconds 0 they do not
        (docs/WorkerProcess.md sec 59)"""
        base, left, right, tails = self.splitChains()
        join = self.counted("Join")
        join.setExpression("Input", "LeftTail.Result + RightTail.Result")
        self.doc.recompute()  # its time known
        self.assertEqual(join.Result, 32)
        base.Input = 2
        began = time.time()
        info = self.byWorker()
        took = time.time() - began
        self.assertEqual(info["Split"], 1, info)
        self.assertEqual(info["SplitJoins"], 3, info)  # Join and the tails
        self.assertEqual(info["JoinedHere"], 3, info)
        self.assertEqual(info["ExecutedHere"], 0, info)
        self.assertGreaterEqual(info["SplitSaving"], 1.4, info)
        self.assertEqual(self.touched(), [])
        self.assertEqual((left.Result, right.Result), (8, 8))
        self.assertEqual([t.Result for t in tails], [16, 16])
        self.assertEqual(join.Result, 64)
        self.assertNotIn(os.getpid(), (left.MadeBy, right.MadeBy))
        self.assertNotEqual(left.MadeBy, right.MadeBy)
        self.assertEqual([t.MadeBy for t in tails] + [join.MadeBy], [os.getpid()] * 3)
        self.assertLess(took, 2.0 + 1.5, info)
        # No joins: one part, as before
        group = App.ParamGet("User parameter:BaseApp/Preferences/Workers")
        group.SetFloat("JoinSeconds", 0.0)
        self.addCleanup(group.RemFloat, "JoinSeconds")
        base.Input = 3
        info = self.byWorker()
        self.assertEqual(info["Split"], 1, info)  # still the one before
        self.assertEqual(left.MadeBy, right.MadeBy)
        self.assertEqual(join.Result, 96)
        self.assertNotEqual(join.MadeBy, os.getpid())

    def testATechDrawViewIsMadeByAWorker(self):
        """A TechDraw view is made by a worker -- inline, a console process --
        and put in from its stored projection: the same edges as made here,
        and not executed here (docs/WorkerProcess.md sec 39)"""
        import TechDraw  # noqa: F401

        box = self.doc.addObject("Part::Box", "Box")
        page = self.doc.addObject("TechDraw::DrawPage", "Page")
        template = self.doc.addObject("TechDraw::DrawSVGTemplate", "Template")
        template.Template = os.path.join(
            App.getHomePath(), "Mod", "TechDraw", "TDTest", "TestTemplate.svg")
        page.Template = template
        view = self.doc.addObject("TechDraw::DrawViewPart", "View")
        page.addView(view)
        view.Source = [box]
        view.Direction = App.Vector(1, 1, 1)
        self.doc.recompute()

        def edges():
            return sorted(round(e.Length, 6) for e in view.getVisibleEdges())

        # Made here, inline: a console recompute makes a view at once
        self.assertEqual(len(edges()), 9, edges())
        box.Length = 25
        info = self.byWorker()
        self.assertEqual(info["Applied"], 1, info)
        self.assertEqual(info["MadeHere"], 0, info)
        self.assertEqual(info["ExecutedHere"], 0, info)
        self.assertEqual(self.touched(), [])
        made = edges()
        self.assertEqual(len(made), 9, made)
        # The same as this process makes of it
        view.touch()
        self.doc.recompute()
        self.assertEqual(edges(), made)
        # Not stored, it cannot come in values: made here
        group = App.ParamGet("User parameter:BaseApp/Preferences/Mod/TechDraw/General")
        group.SetBool("StoreProjectedGeometry", False)
        self.addCleanup(group.RemBool, "StoreProjectedGeometry")
        box.Length = 30
        info = self.byWorker()
        self.assertEqual(info["MadeHere"], 1, info)
        self.assertEqual(len(edges()), 9)

    def testAViewsDimensionsComeBackWithIt(self):
        """A TechDraw view recomputes its dimensions in its execute(), and a
        dimension finds its references again there when the projection
        numbers its elements otherwise. Put in rather than executed here,
        the view leaves them to this process's recompute, which does not
        reach them -- they are not touched: they are recomputed after it,
        each from what the worker made of it. Found again by the geometry it
        saved, read by the worker from a version of the document, whose
        files are named by content too (docs/WorkerProcess.md sec 46)"""
        import shutil

        import TechDraw  # noqa: F401

        box = self.doc.addObject("Part::Box", "Box")
        # Made by the worker and not sent, read by the model (sec 44): its
        # entry is left out of what comes back, and the places counted past
        # it are counted again
        sheet = self.doc.addObject("Spreadsheet::Sheet", "Sheet")
        sheet.set("A1", "10")
        sheet.setAlias("A1", "size")
        box.setExpression("Length", "Sheet.size")
        other = self.doc.addObject("Part::Box", "Other")
        other.Length = other.Width = other.Height = 4
        other.Placement.Base = App.Vector(40, 0, 0)
        page = self.doc.addObject("TechDraw::DrawPage", "Page")
        template = self.doc.addObject("TechDraw::DrawSVGTemplate", "Template")
        template.Template = os.path.join(
            App.getHomePath(), "Mod", "TechDraw", "TDTest", "TestTemplate.svg")
        page.Template = template
        # A view of a projection group: the group's execute() recomputes it,
        # and it its dimensions
        group = self.doc.addObject("TechDraw::DrawProjGroup", "Group")
        page.addView(group)
        group.Source = [box, other]
        view = group.addProjection("Front")
        # Drawn first, as in the GUI: a dimension made with its view saves
        # its references before there is anything to save
        self.doc.recompute()
        dim = self.doc.addObject("TechDraw::DrawViewDimension", "Dim")
        dim.Type = "DistanceX"
        dim.References2D = [(view, ("Vertex0", "Edge1"))]
        page.addView(dim)
        self.doc.recompute()
        self.assertAlmostEqual(dim.getRawValue(), 10.0)
        # Found by geometry alone, not by the names its elements carry
        dim.SavedNames = ["", ""]
        # Read from a file, as a worker reads the files of a version
        path = os.path.join(tempfile.mkdtemp(prefix="fc-dims-"), "dims.FCStd")
        self.addCleanup(shutil.rmtree, os.path.dirname(path), True)
        self.doc.saveAs(path)
        name = self.doc.Name
        App.closeDocument(name)
        self.doc = App.openDocument(path)
        self.pool = App.workerPool(self.doc)
        group = self.doc.getObject("Group")
        dim = self.doc.getObject("Dim")
        box, other = self.doc.getObject("Box"), self.doc.getObject("Other")
        self.assertEqual(dim.References2D[0][1], ("Vertex0", "Edge1"))
        # The same outline, numbered otherwise: the other box's first
        group.Source = [other, box]
        self.doc.getObject("Sheet").touch()
        info = self.byWorker()
        self.assertEqual(info["Applied"], 1, info)
        self.assertEqual(info["ExecutedHere"], 0, info)
        self.assertEqual(info["Unused"], 0, info)
        self.assertGreaterEqual(info["Inner"], 1, info)
        self.assertNotEqual(dim.References2D[0][1], ("Vertex0", "Edge1"))
        self.assertAlmostEqual(dim.getRawValue(), 10.0)
        self.assertEqual(self.touched(), [])

    def polygonSketch(self):
        """A sketch projecting the face of a _Polygon, made here"""
        import Sketcher  # noqa: F401

        poly = self.doc.addObject("Part::FeaturePython", "Poly")
        _Polygon(poly)
        self.doc.recompute()
        sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        sketch.addExternal("Poly", "Face1")
        self.doc.recompute()
        return poly, sketch

    @staticmethod
    def externalIds(sketch):
        import Sketcher

        return [Sketcher.ExternalGeometryFacade(g).Id for g in sketch.ExternalGeo[2:]]

    def testASketchsNewGeometryIdsAreTakenFromAWorker(self):
        """A sketch whose projection comes out in more pieces gives the new
        ones new ids: a worker gives them from what this process had given,
        and this process takes them, and counts on from them
        (docs/WorkerProcess.md sec 36)"""
        import Part

        poly, sketch = self.polygonSketch()
        before = self.externalIds(sketch)
        self.assertEqual(len(before), 4, before)
        poly.Sides = 6
        info = self.byWorker()
        self.assertEqual(info["Unfit"], 0, info)
        self.assertEqual(info["Applied"], 1, info)
        self.assertEqual(info["ExecutedHere"], 0, info)
        self.assertNotEqual(poly.MadeBy, os.getpid())
        after = self.externalIds(sketch)
        self.assertEqual(len(after), 6, after)
        self.assertEqual(len(set(after)), 6, after)
        given = sorted(set(after) - set(before))
        self.assertEqual(len(given), 2, (before, after))
        self.assertGreater(min(given), max(before), (before, after))
        self.assertNotIn("Touched", sketch.State)
        # This process counts on from what the worker gave
        sketch.addGeometry(Part.LineSegment(App.Vector(0, 0, 0), App.Vector(1, 1, 0)))
        self.assertGreater(sketch.getGeometryId(0), max(after), (sketch.getGeometryId(0), after))

    def testAWorkerCountsOnFromIdsGivenHereAndUndone(self):
        """A worker gives no id this process gave, even to pieces since
        undone, which a document saved before does not know of: it counts on
        from what this process tells it (sec 36)"""
        self.doc.UndoMode = 1
        poly, sketch = self.polygonSketch()
        held = self.externalIds(sketch)
        self.doc.openTransaction("here")
        poly.Sides = 6
        self.doc.recompute()
        here = self.externalIds(sketch)
        self.doc.commitTransaction()
        self.assertEqual(len(here), 6, here)
        self.doc.undo()
        self.doc.recompute()
        self.assertEqual(self.externalIds(sketch), held)
        poly.Sides = 6
        info = self.byWorker()
        self.assertEqual(info["Unfit"], 0, info)
        self.assertEqual(info["ExecutedHere"], 0, info)
        self.assertNotEqual(poly.MadeBy, os.getpid())
        there = self.externalIds(sketch)
        self.assertEqual(len(there), 6, there)
        self.assertGreater(min(set(there) - set(held)), max(here), (held, here, there))

    def testAQuietDocumentsWorkersMoveBeforeTheNextJob(self):
        """A recompute made here leaves the workers' copy behind. Once the
        document is quiet for Workers/MoveWhenIdleSeconds, the newer copy is
        made and the workers moved to it, before a job waits for both: on
        scanner that move was 1.17 s of the next job (docs/WorkerProcess.md
        sec 57). Only where an event loop runs, as in the GUI: pumped here"""
        from PySide import QtCore

        group = App.ParamGet("User parameter:BaseApp/Preferences/Workers")
        group.SetFloat("MoveWhenIdleSeconds", 0.3)
        self.addCleanup(group.RemFloat, "MoveWhenIdleSeconds")
        first = self.counted("First", Input=1)
        second = self.counted("Second")
        second.setExpression("Input", "First.Result")
        self.doc.recompute()
        first.Input = 2
        self.byWorker()  # a worker up, on the copy of then
        self.assertEqual(second.Result, 8)
        info = self.pool.info()
        copies, moves = info["IdleCopies"], info["Moves"]

        def pump(until, seconds=10):
            end = time.time() + seconds
            while time.time() < end and not until():
                QtCore.QCoreApplication.processEvents()
                self.pool.poll(20)
            return until()

        # Not while a transaction is open: it is a command's
        self.doc.UndoMode = 1
        self.doc.openTransaction("an edit")
        first.Input = 3
        self.doc.recompute()  # made here
        self.assertFalse(pump(lambda: self.pool.info()["IdleCopies"] > copies, 1.0))
        self.doc.commitTransaction()
        self.assertTrue(pump(lambda: self.pool.info()["IdleCopies"] > copies), self.pool.info())
        self.assertEqual(self.pool.info()["IdleCopies"], copies + 1)
        # the worker told, and there
        self.assertTrue(pump(lambda: self.pool.info()["Moves"] > moves), self.pool.info())
        # quiet and the copy current: nothing more is made
        self.assertFalse(pump(lambda: self.pool.info()["IdleCopies"] > copies + 1, 1.0))
        # the next job is made on it, and right
        first.Input = 4
        info = self.byWorker()
        self.assertEqual(info["Applied"], 2, info)
        self.assertEqual(second.Result, 16)
        self.assertEqual(self.touched(), [])

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
        # Where its time went (docs/WorkerProcess.md sec 53): each part on
        # this process's clock, adding up to the whole
        times = info["Times"]
        self.assertTrue(all(v >= 0 for v in times.values()), times)
        self.assertGreater(times["Job"], 0, times)
        self.assertLessEqual(times["Job"], times["Returned"] + 0.05, times)
        self.assertLessEqual(times["Setup"], times["Queued"] + 0.05, times)
        parts = sum(times[k] for k in ("Start", "Queued", "Returned", "Held", "Read", "Apply"))
        self.assertAlmostEqual(times["Total"], parts, delta=0.05)
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

    def testAWorkerKeepsWhatItMadeAsItMovesAcrossIt(self):
        """The next move of a worker crosses the rows its own result was put
        in as: what it sent it keeps as it holds it, not read back as this
        process wrote it again (docs/WorkerProcess.md sec 29). The edit
        after it is still taken, and what it makes next is what a recompute
        here makes"""
        import Part  # noqa: F401

        box = self.doc.addObject("Part::Box", "Box")
        tool = self.doc.addObject("Part::Cylinder", "Tool")
        tool.Radius = 2
        tool.Height = 30
        tool.Placement = App.Placement(App.Vector(6, 3, -5), App.Rotation(App.Vector(0, 0, 1), 37))
        cut = self.doc.addObject("Part::Cut", "Cut")
        cut.Base = box
        cut.Tool = tool
        counted = self.counted("Counted")
        counted.setExpression("Input", "Box.Height")
        self.doc.recompute()
        box.Length = 14
        info = self.byWorker()
        self.assertEqual(info["Applied"], 1, info)
        self.assertEqual(self.pool.info()["Kept"], 0)
        made = []
        for height in (12, 9):
            box.Height = height  # an edit of what the worker made
            info = self.byWorker()
            self.assertEqual(info["Applied"], len(made) + 2, info)
            self.assertEqual(info["ExecutedHere"], 0, info)
            self.assertEqual(counted.Result, 2 * height)
            made.append((cut.Shape.Volume, len(cut.Shape.Faces),
                         sorted(cut.Shape.ElementMap.items())))
            self.assertAlmostEqual(made[-1][0], 14 * 10 * height - 3.141592653589793 * 4 * height,
                                   6)
        pool = self.pool.info()
        self.assertGreaterEqual(pool["Moves"], 2, pool)
        self.assertGreater(pool["Kept"], 0, pool)
        # The same made here
        box.Height = 10
        self.doc.recompute()
        box.Height = 9
        self.doc.recompute()
        self.assertEqual((cut.Shape.Volume, len(cut.Shape.Faces),
                          sorted(cut.Shape.ElementMap.items())), made[-1])

    def testARecomputeIsExpectedToTakeWhatItsObjectsTookLast(self):
        """What a recompute of what is touched would take: what each object
        it reaches took when last made, here or by a worker
        (docs/WorkerProcess.md sec 28)"""
        self.pool.expectedRecompute()  # noting from now
        slow = self.counted("Slow", Sleep=0.3, Input=1)
        after = self.counted("After")
        after.setExpression("Input", "Slow.Result")
        quick = self.counted("Quick")
        self.assertEqual(self.pool.expectedRecompute()[1], 3)  # never made
        self.doc.recompute()
        slow.Input = 2  # reaches After
        seconds, unknown = self.pool.expectedRecompute()
        self.assertEqual(unknown, 0)
        self.assertGreaterEqual(seconds, 0.3)
        self.assertLess(seconds, 0.6)
        self.doc.recompute()
        quick.Input = 5
        self.assertLess(self.pool.expectedRecompute()[0], 0.1)
        self.doc.recompute()
        # Made by a worker: its times
        slow.Sleep = 0.8
        info = self.byWorker()
        self.assertEqual(info["Applied"], 1, info)
        slow.Input = 3
        seconds, unknown = self.pool.expectedRecompute()
        self.assertEqual(unknown, 0)
        self.assertGreaterEqual(seconds, 0.8)
        self.assertLess(seconds, 1.2)

    def testAnOpenKnowsWhatARecomputeTookFromTheLog(self):
        """After an open, an object not made since is expected to take what
        the log's recompute records say it took (docs/WorkerProcess.md
        sec 30)"""
        import shutil

        folder = tempfile.mkdtemp(prefix="fc-times-")
        self.addCleanup(shutil.rmtree, folder, True)
        slow = self.counted("Slow", Sleep=0.4, Input=1)
        after = self.counted("After")
        after.setExpression("Input", "Slow.Result")
        self.doc.recompute()
        slow.Input = 2
        self.doc.recompute()
        path = os.path.join(folder, "times.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        self.pool = App.workerPool(self.doc)
        slow = self.doc.getObject("Slow")
        slow.Input = 3  # nothing made since the open
        seconds, unknown = self.pool.expectedRecompute()
        self.assertEqual(unknown, 0)
        self.assertGreaterEqual(seconds, 0.4)
        self.assertLess(seconds, 0.8)

    def testWhatIsMarkedHereIsMadeThere(self):
        """An object marked here to be recomputed (enforceRecompute), which
        nothing in the log says, is recomputed by the worker too
        (docs/WorkerProcess.md sec 31)"""
        marked = self.counted("Marked", Input=3)
        self.doc.recompute()
        marked.enforceRecompute()
        info = self.byWorker()
        self.assertEqual(info["Applied"], 1, info)
        self.assertEqual(info["Supplied"], 1, info)
        self.assertNotEqual(marked.MadeBy, os.getpid())
        self.assertEqual(marked.Result, 6)

    def testAWorkersLaterStringsAreSavedAndReadBack(self):
        """A worker gives its strings ids of a range leased it at its first
        job. Strings made here since have ids above that range; what the
        worker builds on them at a later job takes ids above them too, of
        a new range -- a string of an id below what it refers to is one a
        saved table was not read back with (docs/WorkerProcess.md sec 32)"""
        import re

        import Part  # noqa: F401

        def fillet(name, size, at):
            box = self.doc.addObject("Part::Box", name)
            box.Length = box.Width = size
            box.Height = 20
            box.Placement.Base = App.Vector(at, at, -5 if at else 0)
            made = self.doc.addObject("Part::Fillet", name + "Fillet")
            made.Base = box
            made.Edges = [(1, 1.0, 1.0), (5, 1.0, 1.0)]
            return made

        first = fillet("Box", 10, 0)
        self.doc.recompute()
        first.touch()
        self.assertEqual(self.byWorker()["Applied"], 1)  # the worker has its ids
        second = fillet("Tool", 4, 3)
        self.doc.recompute()  # made here: ids above the worker's
        cut = self.doc.addObject("Part::Cut", "Cut")
        cut.Base = first
        cut.Tool = second
        info = self.byWorker()
        self.assertEqual(info["Applied"], 2, info)
        self.assertTrue(info["ById"], info)
        self.assertTrue(cut.Shape.isValid())
        hasher = self.doc.Hasher
        table = hasher.Table
        self.assertGreater(len(table), 60)
        forward = []
        for sid in sorted(table):
            related = [r if isinstance(r, int) else r.Value for r in hasher.getID(sid).Related]
            if [r for r in related if r > sid]:
                forward.append((sid, related))
        self.assertEqual(forward, [])
        names = dict(cut.Shape.ElementMap)
        path = os.path.join(tempfile.gettempdir(), "TestWorkerLaterStrings.FCStd")
        self.addCleanup(lambda: os.path.exists(path) and os.remove(path))
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        self.pool = App.workerPool(self.doc)
        cut = self.doc.getObject("Cut")
        self.assertEqual(dict(cut.Shape.ElementMap), names)
        hasher = self.doc.Hasher
        spelled = set(m for name in names for m in re.findall(r"#([0-9a-f]+)", name))
        self.assertGreater(len(spelled), 5)
        self.assertEqual([s for s in spelled if not hasher.getIDBySpelling(int(s, 16))], [])

    def crashingChain(self):
        """Before -> Crash -> After: Crash's execute() takes a worker down"""
        before = self.counted("Before", Input=2)
        crash = self.doc.addObject("App::FeaturePython", "Crash")
        _Crashing(crash)
        crash.setExpression("Input", "Before.Result")
        after = self.counted("After")
        after.setExpression("Input", "Crash.Result")
        _executions.pop("crashed here", None)
        return before, crash, after

    def testACrashInAWorkerIsPutDownToItsObject(self):
        """A worker that dies in an object's execute() names it: the object
        is left in an error that says so, and a recompute here does not run
        it (docs/WorkerProcess.md sec 34)"""
        before, crash, after = self.crashingChain()
        self.assertTrue(self.pool.recompute(), self.pool.recomputeInfo()["Reason"])
        self.assertTrue(self.pool.waitRecompute(120), self.pool.recomputeInfo())
        info = self.pool.recomputeInfo()
        self.assertEqual(info["Failed"], 1, info)
        self.assertEqual(info["Crashed"], ["Crash"], info)
        self.assertIn("at Crash", info["Reason"])
        self.assertIn("crashed", info["Reason"])
        self.assertIn("Invalid", crash.State)
        self.assertIn("crashed a worker process", crash.getStatusString())
        self.assertIn("What was on the worker's stack is in ", crash.getStatusString())
        # Here: what comes before is made, the crashing one is not run, and
        # what reads it is not made from it
        self.doc.recompute()
        self.assertNotIn("crashed here", _executions)
        self.assertEqual(before.Result, 4)
        self.assertEqual(before.MadeBy, os.getpid())
        self.assertIn("Invalid", crash.State)
        self.assertEqual(after.Result, 0)
        # Its own recompute is refused as well
        crash.recompute()
        self.assertNotIn("crashed here", _executions)
        # Given to a worker again, as it is: not run there either -- no
        # worker dies, and it comes back in the same error
        crash.touch()
        failed = self.pool.info()["Failed"]
        pids = set(self.pool.info()["Pids"])
        info = self.byWorker()
        self.assertEqual(info["Failed"], 1, info)
        self.assertGreaterEqual(info["Applied"], 1, info)
        self.assertIn("Invalid", crash.State)
        self.assertIn("crashed a worker process twice (0xc0000005" if os.name == "nt"
                      else "crashed a worker process twice (signal 11", crash.getStatusString())
        self.assertEqual(self.pool.info()["Failed"], failed)
        self.assertTrue(pids <= set(self.pool.info()["Pids"]), self.pool.info()["Pids"])

    @staticmethod
    def systemCrashReports():
        """The system's crash reports of this build's processes: macOS's, and
        Windows Error Reporting's reports and dumps (sec 41)"""
        import glob
        import sys

        if sys.platform == "win32":
            found = set()
            for root in (os.environ.get("LOCALAPPDATA", ""), os.environ.get("ProgramData", "")):
                if not root:
                    continue
                for where in (
                    "Microsoft/Windows/WER/ReportArchive",
                    "Microsoft/Windows/WER/ReportQueue",
                ):
                    found.update(glob.glob(os.path.join(root, where, "*Thundereal*")))
            found.update(glob.glob(os.path.join(os.environ.get("LOCALAPPDATA", ""),
                                                "CrashDumps", "Thundereal*")))
            return found
        if sys.platform != "darwin":
            return set()
        return set(glob.glob(os.path.expanduser("~/Library/Logs/DiagnosticReports/Thundereal*")))

    def testACrashSaysWhatWasOnItsStack(self):
        """A worker that crashes says what was on its stack -- the signal, the
        frames of the thread that crashed, Python's traceback -- which the
        result has and the crash log keeps, then with the frames' lines; no
        system crash report is made of it (docs/WorkerProcess.md sec 35)"""
        import shutil
        import sys

        reports = self.systemCrashReports()
        self.assertTrue(self.pool.start())
        job = self.pool.submit("test.fault", {}, module=MODULE)
        self.assertGreater(job, 0, self.pool.info()["Reason"])
        self.assertTrue(self.pool.wait(120), self.pool.info())
        result = self.pool.result(job)
        self.assertEqual(result["Status"], "Failed", result)
        self.assertTrue(result["Died"], result)
        if sys.platform == "win32":
            # Told by the exception's code, as Windows tells it (sec 41)
            self.assertEqual(result["How"], "crashed (0xc0000005)", result)
        else:
            self.assertRegex(result["How"], r"^crashed \(signal (11|10): ", result)
        stack = result["Stack"]
        self.assertIn("at address 0x0", stack)
        # Python's traceback, and the native frames from the fault down. On
        # Windows the worker names a frame only by an export near it, and
        # FreeCADApp.dll exports few: its frames are named from the PDB, in
        # the lines' entry below (sec 41)
        self.assertIn("in _fault", stack)
        if sys.platform == "win32":
            self.assertIn("(FreeCADApp.dll +0x", stack)
        else:
            self.assertIn("App::WorkerPool::serve()", stack)
        log = result["CrashLog"]
        self.assertTrue(log and os.path.exists(log), result)
        self.assertIn(log, result["Error"])
        title = "crashed making job %d (test.fault)" % job
        with open(log, errors="replace") as f:
            self.assertIn(title, f.read())
        if sys.platform == "darwin":
            tool = any(os.path.exists(p) for p in (
                "/Library/Developer/CommandLineTools/usr/bin/atos",
                "/Applications/Xcode.app/Contents/Developer/usr/bin/atos"))
        elif sys.platform == "win32":
            # dbghelp, with the PDBs of a build that has them
            tool = os.path.exists(os.path.join(App.getHomePath(), "bin", "FreeCADApp.pdb"))
        else:
            tool = shutil.which("addr2line") is not None
        if tool:
            def lines():
                with open(log, errors="replace") as f:
                    text = f.read()
                at = text.find(title + ", the frames with their lines")
                return (at >= 0 and "WorkerPool.cpp:" in text[at:]
                        and "App::WorkerPool::serve" in text[at:])
            end = time.time() + 60
            while not lines() and time.time() < end:
                time.sleep(0.2)
            self.assertTrue(lines(), log)
        time.sleep(2)  # the system's reports are written after the process goes
        self.assertEqual(self.systemCrashReports(), reports)

    def testAChangeToWhatItReadsHasItTriedAgainByAWorker(self):
        """A change to the crashed object, or to what it reads, lifts the
        refusal; it is then tried in a worker first (sec 34)"""
        before, crash, after = self.crashingChain()
        self.byWorker()
        self.assertEqual(self.pool.recomputeInfo()["Crashed"], ["Crash"])
        self.doc.recompute()
        # A change that reaches nothing of it does not lift it
        other = self.counted("Other", Input=1)
        self.doc.recompute()
        self.assertEqual(self.pool.recomputeInfo()["Crashed"], ["Crash"])
        # What it reads changed: tried again -- and made, the crash fixed
        crash.Crash = False
        self.assertEqual(self.pool.recomputeInfo()["Crashed"], [])
        self.assertTrue(crash.isValid())
        before.Input = 3
        info = self.byWorker()
        self.assertGreaterEqual(info["Applied"], 1, info)
        self.assertEqual(crash.Result, 12)
        self.assertNotEqual(crash.MadeBy, os.getpid())
        self.assertEqual(after.Result, 24)
        self.assertNotIn("crashed here", _executions)
        self.assertEqual(other.Result, 2)

    def testWhatCrashedIsTrustedAgainAfterCleanRuns(self):
        """An object that crashed a worker and was tried again is handed to a
        worker at each recompute reaching it -- until a worker has made it
        Workers/RiskyClearAfter times, 3 by default, with no crash: then it is
        made here as any other object (docs/WorkerProcess.md sec 56)"""
        before, crash, after = self.crashingChain()
        self.byWorker()
        self.assertEqual(self.pool.recomputeInfo()["Crashed"], ["Crash"])
        crash.Crash = False
        handed = self.pool.recomputeInfo()["HandedOver"]
        for k in range(3):
            self.assertEqual(self.pool.recomputeInfo()["Risky"], ["Crash"], k)
            before.Input = 10 + k
            self.doc.recompute()
            info = self.pool.recomputeInfo()
            self.assertEqual(info["HandedOver"], handed + k + 1, info)
            self.assertNotEqual(crash.MadeBy, os.getpid())
        self.assertEqual(self.pool.recomputeInfo()["Risky"], [])
        before.Input = 20
        self.doc.recompute()
        self.assertEqual(self.pool.recomputeInfo()["HandedOver"], handed + 3)
        self.assertEqual(crash.MadeBy, os.getpid())
        self.assertEqual(crash.Result, 80)
        self.assertNotIn("crashed here", _executions)

    def testAScriptsRecomputeOfWhatCrashedIsAWorkersFirst(self):
        """A recompute a script asks of this process, reaching an object that
        crashed a worker and was changed since, is made by a worker first:
        a crash again costs a worker, not this process; fixed, it is made
        there, and what is left here (docs/WorkerProcess.md sec 37)"""
        before, crash, after = self.crashingChain()
        self.byWorker()
        self.assertEqual(self.pool.recomputeInfo()["Crashed"], ["Crash"])
        handed = self.pool.recomputeInfo()["HandedOver"]
        # Still refused: not handed over, nothing runs it
        self.doc.recompute()
        self.assertEqual(self.pool.recomputeInfo()["HandedOver"], handed)
        # Changed, not fixed: the script's recompute goes to a worker, which
        # dies in it twice, and it is refused again
        crash.Sleep = 0.01
        self.assertEqual(self.pool.recomputeInfo()["Crashed"], [])
        failed = self.pool.info()["Failed"]
        self.doc.recompute()
        info = self.pool.recomputeInfo()
        self.assertEqual(info["HandedOver"], handed + 1, info)
        self.assertEqual(info["Crashed"], ["Crash"], info)
        self.assertEqual(self.pool.info()["Failed"], failed + 1, self.pool.info())
        self.assertNotIn("crashed here", _executions)
        self.assertIn("Invalid", crash.State)
        self.assertEqual(before.Result, 4)
        # Fixed: made by a worker, and what reads it with it
        crash.Crash = False
        before.Input = 3
        self.doc.recompute()
        info = self.pool.recomputeInfo()
        self.assertEqual(info["HandedOver"], handed + 2, info)
        self.assertEqual(crash.Result, 12)
        self.assertNotEqual(crash.MadeBy, os.getpid())
        self.assertEqual(after.Result, 24)
        self.assertNotIn("crashed here", _executions)
        self.assertEqual(self.touched(), [])
        # Off: made here, as before
        group = App.ParamGet("User parameter:BaseApp/Preferences/Workers")
        group.SetBool("HandOverRisky", False)
        self.addCleanup(group.RemBool, "HandOverRisky")
        before.Input = 4
        self.doc.recompute()
        self.assertEqual(self.pool.recomputeInfo()["HandedOver"], handed + 2)
        self.assertEqual(crash.MadeBy, os.getpid())

    def testOneObjectsRecomputeOfWhatCrashedIsAWorkersFirst(self):
        """The recompute of one object -- a task dialog's preview, a script's
        obj.recompute() -- of one that crashed a worker and was changed since
        is made by a worker first too; one still refused is not run anywhere
        (docs/WorkerProcess.md sec 37)"""
        before, crash, after = self.crashingChain()
        self.byWorker()
        handed = self.pool.recomputeInfo()["HandedOver"]
        self.assertFalse(crash.recompute())
        self.assertEqual(self.pool.recomputeInfo()["HandedOver"], handed)
        self.assertNotIn("crashed here", _executions)
        # Changed, not fixed: a worker dies in it, it is refused again
        crash.Sleep = 0.01
        failed = self.pool.info()["Failed"]
        self.assertFalse(crash.recompute())
        info = self.pool.recomputeInfo()
        self.assertEqual(info["HandedOver"], handed + 1, info)
        self.assertEqual(info["Crashed"], ["Crash"], info)
        self.assertEqual(self.pool.info()["Failed"], failed + 1, self.pool.info())
        self.assertNotIn("crashed here", _executions)
        # Fixed: made by a worker
        crash.Crash = False
        self.assertTrue(crash.recompute())
        info = self.pool.recomputeInfo()
        self.assertEqual(info["HandedOver"], handed + 2, info)
        self.assertEqual(crash.Result, 8)
        self.assertNotEqual(crash.MadeBy, os.getpid())
        self.assertNotIn("crashed here", _executions)
        # Off: made here
        group = App.ParamGet("User parameter:BaseApp/Preferences/Workers")
        group.SetBool("HandOverRisky", False)
        self.addCleanup(group.RemBool, "HandOverRisky")
        crash.Sleep = 0.02
        self.assertTrue(crash.recompute())
        self.assertEqual(self.pool.recomputeInfo()["HandedOver"], handed + 2)
        self.assertEqual(crash.MadeBy, os.getpid())

    def testACrashAgainAfterAChangeIsRefusedAgain(self):
        """Tried again on a change that does not fix it: it crashes the
        worker again, and is refused here again"""
        before, crash, after = self.crashingChain()
        self.byWorker()
        before.Input = 5
        self.assertEqual(self.pool.recomputeInfo()["Crashed"], [])
        info = self.byWorker()
        self.assertEqual(info["Crashed"], ["Crash"], info)
        self.doc.recompute()
        self.assertNotIn("crashed here", _executions)
        self.assertEqual(before.Result, 10)

    def testADeathThatDoesNotComeAgainRefusesNothing(self):
        """A worker ended from outside in an object's execute() -- for want of
        memory, say -- and the job made the second time: nothing is refused,
        it was not the object's"""
        before = self.counted("Before", Input=2, Sleep=3.0)
        self.assertTrue(self.pool.recompute(), self.pool.recomputeInfo()["Reason"])
        # Killed while it sleeps in Before's execute(): that is in an object.
        # Wait for it to be in it, then take the worker down from outside
        deadline = time.time() + 30
        while self.pool.info()["Busy"] == 0 and time.time() < deadline:
            self.pool.poll(50)
        time.sleep(0.5)
        for pid in self.pool.info()["Pids"]:
            os.kill(pid, 9)
        self.pool.waitRecompute(60)
        info = self.pool.recomputeInfo()
        # Retried once, made the second time
        self.assertEqual(info["Crashed"], [], info)
        self.assertEqual(info["Applied"], 1, info)
        self.assertEqual(before.Result, 4)

    def testAFailureTheWorkerSawIsSaidOnce(self):
        """The worker's "Failed to recompute" is not said here: the put-in
        says it, as a recompute here does (docs/WorkerProcess.md sec 27)"""
        feature = self.doc.addObject("App::FeatureTest", "Failing")
        self.doc.recompute()
        feature.ExceptionType = 2
        heard = []

        def observer(notifier, message, level):
            if level == "Error" and "Failed to recompute" in message:
                heard.append((notifier, message))

        App.Console.AttachObserver(observer)
        self.addCleanup(App.Console.DetachObserver, observer)
        info = self.byWorker()
        self.assertEqual(info["Applied"], 1, info)
        self.assertEqual(len(heard), 1, heard)
        self.assertNotIn("worker", heard[0][0])

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

    def testAWorkerDecidesPermissionsAsHere(self):
        """An expression's permissions are decided in the worker as here
        (docs/WorkerProcess.md sec 23): a session grant here, which no
        grants.json holds, lets the worker make it; once taken back, the
        worker refuses, the object is left to be made here, and here the
        user is asked."""
        security = App.ExpressionSecurity
        counted = self.counted("Asker")
        # An app.query call both engines answer alike: the native one, and
        # the sandbox's image where a runtime is installed. A worker's copy
        # has the document's name
        counted.setExpression("Input", "_app.getDocument(<<%s>>).Name.__len__()" % self.doc.Name)
        principal = security.principalOf(self.doc.Name)
        self.addCleanup(security.revoke, principal, "app.query", "*")
        self.addCleanup(security.clearPending, principal, "app.query", "*")
        # An answer the profile keeps for this principal -- every new empty
        # document has the same one -- goes first: the test grants its own
        security.revoke(principal, "app.query", "*")
        security.grant(principal, "app.query", "*", True, "session")
        self.doc.recompute()
        expected = 2 * len(self.doc.Name)
        self.assertEqual(counted.Result, expected)

        # A write the expression overrides: evaluated again there (a touch
        # alone evaluates no expression)
        counted.Input = 0
        info = self.byWorker()
        self.assertEqual(info["Applied"], 1, info)
        self.assertEqual(info["ExecutedHere"], 0, info)
        self.assertNotEqual(counted.MadeBy, os.getpid())
        self.assertEqual(counted.Result, expected)
        self.assertEqual(self.touched(), [])

        security.revoke(principal, "app.query", "*")
        counted.Input = 0
        self.assertTrue(self.pool.recompute(), self.pool.recomputeInfo()["Reason"])
        self.assertTrue(self.pool.waitRecompute(120))
        info = self.pool.recomputeInfo()
        self.assertEqual(info["Unfit"], 1, info)
        self.assertIn("permission", info["Reason"])
        self.assertEqual(self.touched(), ["Asker"])
        self.assertEqual([p for p in security.pending() if p["principal"] == principal], [])
        self.doc.recompute()  # here: refused, and asked
        self.assertIn("Invalid", counted.State)
        self.assertEqual(
            [p["permission"] for p in security.pending() if p["principal"] == principal],
            ["app.query"],
        )

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


def _stepData(path):
    """A STEP file's DATA section: the header names the time it was written.
    A colour to the 8 bits a document file keeps of it: one not on a 1/255
    step (the default material's 0.9) is written otherwise after a save and
    an open, here as much as in a worker's copy"""
    import re

    with open(path, encoding="latin-1") as f:
        text = f.read()
    text = text[text.index("DATA;"):]

    def colour(match):
        values = [round(float(v) * 255) for v in match.group(2).split(",")]
        return match.group(1) + ",".join(str(v) for v in values) + ")"

    text = re.sub(r"(COLOUR_RGB\('[^']*',)([^)]*)\)", colour, text)
    # A count of the writer's process, not of the file
    return re.sub(r"NEXT_ASSEMBLY_USAGE_OCCURRENCE\('\d+'", "NEXT_ASSEMBLY_USAGE_OCCURRENCE('n'", text)


def _stepGeometry(path):
    """What comes before the colours: OCCT writes those (one presentation
    per shape) in the order of a map hashed by the shapes' addresses
    (STEPCAFControl_Writer, myMapCompMDGPR), which two processes can differ
    in, here as much as in a worker"""
    text = _stepData(path)
    at = text.find("MECHANICAL_DESIGN_GEOMETRIC_PRESENTATION_REPRESENTATION")
    return text if at < 0 else text[:text.rfind("\n", 0, at)]


def _stepLooks(path):
    """The colours read back: each object's label and its faces' colours"""
    import Import

    doc = App.newDocument("WorkerExportRead")
    try:
        Import.insert(path, doc.Name)
        doc.recompute()
        looks = []
        for obj in doc.Objects:
            if obj.isDerivedFrom("Part::Feature") and obj.Shape.Faces:
                colours = [tuple(round(c * 255) for c in m.DiffuseColor[:3])
                           for m in obj.ShapeAppearance]
                looks.append((obj.Label, len(obj.Shape.Faces), tuple(colours)))
        return sorted(looks)
    finally:
        App.closeDocument(doc.Name)


class TestWorkerExport(unittest.TestCase):
    """Kind "export" (docs/WorkerProcess.md sec 24): Module.export() on the
    worker's copy, the file the same as one written here"""

    def setUp(self):
        import Import  # noqa: F401
        import Part  # noqa: F401

        self.doc = App.newDocument("WorkerExport")
        group = App.ParamGet("User parameter:BaseApp/Preferences/Workers")
        group.SetInt("Workers", 1)
        self.addCleanup(group.RemInt, "Workers")
        self.pool = App.workerPool(self.doc)
        self.dir = tempfile.mkdtemp(prefix="fc-wexport-")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def reopened(self):
        """The document saved and opened again here. What a worker writes is
        made from its copy, which is that: the shapes read back share
        their equal geometry, and the writer meets it in another order --
        the same model, a zero's sign moved between two directions"""
        path = os.path.join(self.dir, self.doc.Name + ".FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        return self.doc

    def export(self, objs, name, settings=""):
        path = os.path.join(self.dir, name)
        self.assertTrue(self.pool.start(), self.pool.info()["Reason"])
        job = self.pool.submit("export", {"module": "Import", "objects": [o.Name for o in objs],
                                          "file": path, "options": None, "settings": settings},
                               tag="export", pinned=True)
        self.assertGreater(job, 0, self.pool.info()["Reason"])
        return job, path

    def testTheFileIsTheOneMadeHere(self):
        import Import

        box = self.doc.addObject("Part::Box", "Box")
        cyl = self.doc.addObject("Part::Cylinder", "Cyl")
        cyl.Placement.Base = App.Vector(30, 0, 0)
        link = self.doc.addObject("App::Link", "Link")
        link.LinkedObject = box
        link.Placement.Base = App.Vector(0, 30, 0)
        self.doc.recompute()
        box.ShapeColor = (1.0, 0.0, 0.0)
        faces = box.ShapeAppearance.copy()
        faces.setSize(6)
        look = faces[2]
        look.DiffuseColor = (0.0, 0.0, 1.0, 1.0)
        faces[2] = look
        box.ShapeAppearance = faces
        cyl.ShapeColor = (0.0, 1.0, 0.0)
        link.OverrideMaterial = True
        base = link.ShapeAppearance.Base
        base.DiffuseColor = (0.0, 1.0, 0.0, 1.0)
        link.ShapeAppearance.Base = base
        self.doc.recompute()
        objs = [box, cyl, link]
        job, there = self.export(objs, "there.step")
        self.assertTrue(self.pool.wait(180), self.pool.info())
        result = self.pool.result(job)
        self.assertEqual(result["Status"], "Done", result)
        self.assertEqual(result["Output"]["file"], there)
        doc = self.reopened()
        here = os.path.join(self.dir, "here.step")
        Import.export([doc.getObject(o) for o in ("Box", "Cyl", "Link")], here)
        self.assertEqual(_stepGeometry(there), _stepGeometry(here))
        looks = _stepLooks(there)
        self.assertEqual(looks, _stepLooks(here))
        self.assertIn((255, 0, 0), looks[0][2] + looks[-1][2])
        self.assertTrue(any((0, 0, 255) in look[2] for look in looks), looks)

    def testTheDesignAsItWasAskedFor(self):
        """An edit after it was asked for, a recompute and a newer copy for
        the workers: the file is the design as it was"""
        import Import

        box = self.doc.addObject("Part::Box", "Box")
        self.doc.recompute()
        self.doc = self.reopened()
        box = self.doc.getObject("Box")
        self.pool = App.workerPool(self.doc)
        here = os.path.join(self.dir, "here.step")
        Import.export([box], here)
        # A worker ready on the copy: the export is handed to it at once
        self.assertTrue(self.pool.start())
        ready = self.pool.submit("test.echo", {}, module=MODULE)
        self.assertTrue(ready and self.pool.wait(180), self.pool.info())
        job, there = self.export([box], "there.step")
        self.assertEqual(self.pool.state(job), "Running")
        self.doc.openTransaction("Edit")
        box.Length = 25
        self.doc.commitTransaction()
        self.doc.recompute()
        self.assertTrue(self.pool.start())  # a newer copy, the worker moved after
        self.assertTrue(self.pool.wait(180), self.pool.info())
        result = self.pool.result(job)
        self.assertEqual(result["Status"], "Done", result)
        self.assertEqual(_stepData(there), _stepData(here))

    def testAPreferenceSetSinceTheWorkerStarted(self):
        """The preferences an exporter reads go with the job: the worker's
        own are from when it was started"""
        box = self.doc.addObject("Part::Box", "Box")
        self.doc.recompute()
        self.assertTrue(self.pool.start())
        step = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Part/STEP")
        self.assertEqual(step.GetString("Company", ""), "")
        self.addCleanup(step.RemString, "Company")
        step.SetString("Company", "WorkerExportCompany")
        settings = os.path.join(self.dir, "settings.cfg")
        App.ParamGet("User parameter:BaseApp/Preferences").Export(settings)
        job, there = self.export([box], "there.step", settings)
        self.assertTrue(self.pool.wait(180), self.pool.info())
        self.assertEqual(self.pool.result(job)["Status"], "Done")
        with open(there, encoding="latin-1") as f:
            self.assertIn("WorkerExportCompany", f.read())

    def testMeshAndPartFilesAreTheOnesMadeHere(self):
        """The other modules a worker exports for (ExportModules): Mesh's
        formats and Part's, each the file the reopened document gives here"""
        import Mesh
        import Part

        box = self.doc.addObject("Part::Box", "Box")
        cyl = self.doc.addObject("Part::Cylinder", "Cyl")
        cyl.Placement.Base = App.Vector(30, 0, 0)
        self.doc.recompute()
        self.assertTrue(self.pool.start(), self.pool.info()["Reason"])
        formats = [("Mesh", "stl"), ("Mesh", "obj"), ("Mesh", "3mf"), ("Mesh", "off"),
                   ("Part", "brep"), ("Part", "igs")]
        jobs = []
        for module, ext in formats:
            path = os.path.join(self.dir, "there-%s.%s" % (module, ext))
            jobs.append((self.pool.submit("export", {"module": module,
                                                     "objects": ["Box", "Cyl"],
                                                     "file": path}, pinned=True), path))
        self.assertTrue(self.pool.wait(180), self.pool.info())
        for job, _ in jobs:
            result = self.pool.result(job)
            self.assertEqual(result["Status"], "Done", result)
        doc = self.reopened()
        objs = [doc.getObject("Box"), doc.getObject("Cyl")]
        modules = {"Mesh": Mesh, "Part": Part}
        for (module, ext), (_, there) in zip(formats, jobs):
            here = os.path.join(self.dir, "here-%s.%s" % (module, ext))
            modules[module].export(objs, here)
            with open(there, "rb") as f:
                made = f.read()
            with open(here, "rb") as f:
                want = f.read()
            if ext == "igs":
                # The global section names the time it was written
                made, want = (b"\n".join(l for l in t.split(b"\n") if l[72:73] != b"G")
                              for t in (made, want))
            if ext == "3mf":
                import zipfile

                made, want = (sorted((i.filename, zipfile.ZipFile(p).read(i))
                                     for i in zipfile.ZipFile(p).infolist())
                              for p in (there, here))
            self.assertEqual(made, want, "%s %s" % (module, ext))

    def testAnObjectTheCopyHasNotIsUnfit(self):
        self.doc.addObject("Part::Box", "Box")
        self.doc.recompute()
        self.assertTrue(self.pool.start())
        job = self.pool.submit("export", {"module": "Import", "objects": ["Nothing"],
                                          "file": os.path.join(self.dir, "x.step")},
                               pinned=True)
        self.assertTrue(self.pool.wait(180))
        result = self.pool.result(job)
        self.assertEqual(result["Status"], "Unfit", result)
        self.assertFalse(os.path.exists(os.path.join(self.dir, "x.step")))


class TestWorkerCheck(unittest.TestCase):
    """Kind "part.check" (docs/WorkerProcess.md sec 25): Part_CheckGeometry's
    checks made by a worker, its findings those made here"""

    def setUp(self):
        import Part

        self.doc = App.newDocument("WorkerCheck")
        group = App.ParamGet("User parameter:BaseApp/Preferences/Workers")
        group.SetInt("Workers", 1)
        self.addCleanup(group.RemInt, "Workers")
        self.pool = App.workerPool(self.doc)
        V = App.Vector
        bow = self.doc.addObject("Part::Feature", "Bow")
        bow.Shape = Part.Face(Part.makePolygon([V(0, 0, 0), V(10, 10, 0), V(10, 0, 0),
                                                V(0, 10, 0), V(0, 0, 0)]))
        overlap = self.doc.addObject("Part::Feature", "Overlap")
        overlap.Shape = Part.makeCompound([Part.makeBox(10, 10, 10),
                                           Part.makeBox(10, 10, 10, V(5, 5, 5))])
        self.doc.addObject("Part::Box", "Box")
        self.doc.recompute()

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def testTheFindingsAreTheOnesMadeHere(self):
        import Part

        items = [{"object": "Bow", "sub": ""}, {"object": "Overlap", "sub": ""},
                 {"object": "Box", "sub": "Face1"}]
        for options in ({"bop": False}, {"bop": True}):
            self.assertTrue(self.pool.start(), self.pool.info()["Reason"])
            job = self.pool.submit("part.check", {"items": items, "options": options},
                                   reads=[i["object"] for i in items])
            self.assertGreater(job, 0)
            self.assertTrue(self.pool.wait(180), self.pool.info())
            result = self.pool.result(job)
            self.assertEqual(result["Status"], "Done", result)
            there = [[tuple(f) for f in r["findings"]] for r in result["Output"]["results"]]
            here = [Part.checkGeometry(Part.getShape(self.doc.getObject(i["object"]), i["sub"],
                                                     needSubElement=True), options)
                    for i in items]
            self.assertEqual(there, here, options)
            self.assertTrue(here[0], "the bow tie is invalid")
            self.assertEqual(bool(here[1]), options["bop"], "the overlap, to the BOP check")
            self.assertFalse(here[2])

    def testAnEditOfWhatItChecksDropsIt(self):
        self.assertTrue(self.pool.start())
        ready = self.pool.submit("test.sleep", {"Seconds": 0.5}, module=MODULE)
        job = self.pool.submit("part.check", {"items": [{"object": "Box", "sub": ""}],
                                              "options": {"bop": True}}, reads=["Box"])
        self.doc.openTransaction("Edit")
        self.doc.Box.Length = 20
        self.doc.commitTransaction()
        self.assertEqual(self.pool.result(job)["Status"], "Cancelled")
        self.assertTrue(ready)
