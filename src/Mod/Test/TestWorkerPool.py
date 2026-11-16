# SPDX-License-Identifier: LGPL-2.1-or-later
"""App::WorkerPool on its own (docs/WorkerProcess.md): its lifecycle, a job
handed back exactly once, a worker that dies, a handler that fails or is
unfit, an edit of the design making the copy stale.

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


def _write(doc, inputs, out):
    with open(out, "w") as f:
        f.write(inputs["Text"])
    return {}


for _kind, _handler in (
    ("test.echo", _echo),
    ("test.sleep", _sleep),
    ("test.fail", _fail),
    ("test.unfit", _unfit),
    ("test.dieonce", _dieOnce),
    ("test.read", _read),
    ("test.write", _write),
    ("test.volume", _volume),
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
        self.assertTrue(os.path.isdir(os.path.join(info["Directory"], "version")))
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
        self.assertFalse(os.path.exists(os.path.join(info["Directory"], "version")))
        first = set(os.listdir(info["Blobs"]))
        self.assertGreaterEqual(len(first), 2)  # each shape's
        job = self.submit("test.volume", {"Object": "Box"})
        self.assertTrue(self.pool.wait(120))
        self.assertAlmostEqual(self.pool.result(job)["Output"]["Volume"], 1000.0)
        box.Height = 20  # a new copy: only the box's shape is new
        self.doc.recompute()
        self.assertFalse(self.pool.info()["Started"])
        self.assertTrue(self.pool.start())
        second = set(os.listdir(self.pool.info()["Blobs"]))
        self.assertTrue(first <= second)
        self.assertLessEqual(len(second - first), 2, second - first)
        jobs = [self.submit("test.volume", {"Object": n}) for n in ("Box", "Cylinder")]
        self.assertTrue(self.pool.wait(120))
        self.assertAlmostEqual(self.pool.result(jobs[0])["Output"]["Volume"], 2000.0)
        self.assertAlmostEqual(self.pool.result(jobs[1])["Output"]["Volume"], cyl.Shape.Volume)

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

    def testAnEditMakesTheCopyStale(self):
        self.settings(1)
        self.assertTrue(self.pool.start())
        directory = self.pool.info()["Directory"]
        self.assertTrue(os.path.isdir(directory))
        jobs = [self.submit("test.sleep", {"Seconds": 0.3}) for _ in range(3)]
        self.obj.Integer = 7  # the design changed
        info = self.pool.info()
        self.assertFalse(info["Started"])
        self.assertEqual(info["Workers"], 0)
        self.assertEqual(info["Reason"], "the design changed")
        self.assertFalse(os.path.exists(directory))
        for job in jobs:
            self.assertEqual(self.pool.result(job)["Status"], "Cancelled")
        self.assertEqual(self.pool.submit("test.echo", {}), 0)  # not started
        # A new copy: the design as it is now
        self.assertTrue(self.pool.start())
        job = self.submit("test.read", {"Object": "Feature", "Property": "Integer"})
        self.assertTrue(self.pool.wait(120))
        self.assertEqual(self.pool.result(job)["Output"]["Value"], 7)

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
