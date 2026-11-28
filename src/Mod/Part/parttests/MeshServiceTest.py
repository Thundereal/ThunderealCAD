# SPDX-License-Identifier: LGPL-2.1-or-later
"""Meshes made by a helper process (docs/WorkerProcess.md sec 65).

A shape meshed by a helper holds, bit for bit, the mesh the same mesher
leaves on it here: the faces' triangulations and the polygons of their
edges at every place a face is met, and the polygon of an edge on no face.
A helper that dies over a shape costs only itself: the shape is left
unmeshed, is not sent again, and the next shape is meshed by a new one.
"""

import unittest

import FreeCAD as App
import Part


def shapes():
    """Shapes whose meshes go every way the reply has to carry them"""
    cylinder = Part.makeCylinder(5, 12)  # a seam: polygons on a closed face
    res = {
        "box": Part.makeBox(10, 20, 30),
        "cylinder": cylinder,
        "sphere": Part.makeSphere(7),  # a seam and two poles
        "torus": Part.makeTorus(10, 2),
        "fused": Part.makeBox(10, 20, 30).fuse(
            Part.makeCylinder(4, 40, App.Vector(5, 10, -5))
        ),
        # one solid met at two places: a face record, two locations
        "twice": Part.Compound([cylinder, cylinder.translated(App.Vector(30, 0, 0))]),
        # an edge on no face: its own polygon
        "free edge": Part.Compound([Part.makeBox(2, 2, 2), Part.makeCircle(3)]),
    }
    # placed by a rotation, which BinTools reads back an ulp or so away
    # unless the transformation goes along as it is
    turned = Part.makeCylinder(3, 8)
    turned.Placement = App.Placement(App.Vector(1, 2, 3), App.Rotation(App.Vector(1, 2, 3), 33))
    res["turned"] = Part.Compound([turned, Part.makeBox(1, 1, 1)])
    return res


class MeshServiceTest(unittest.TestCase):
    def setUp(self):
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Workers")
        # The user's setting as it was, set or not
        if "MeshInWorker" in self.params.GetBools():
            self.addCleanup(self.params.SetBool, "MeshInWorker",
                            self.params.GetBool("MeshInWorker"))
        else:
            self.addCleanup(self.params.RemBool, "MeshInWorker")

    def testItIsOnByDefault(self):
        self.params.RemBool("MeshInWorker")
        before = Part.meshServiceInfo()
        self.assertTrue(Part.meshShape(Part.makeBox(1, 1, 1), 0.1, 0.5))
        self.assertEqual(Part.meshServiceInfo()["Worker"], before["Worker"] + 1)

    def meshBoth(self, shape, steps):
        """A copy meshed here and one by a helper, through the same steps"""
        here = shape.copy()
        there = shape.copy()
        for deflection, angle in steps:
            self.assertTrue(Part.meshShape(here, deflection, angle, "here"))
            self.assertTrue(Part.meshShape(there, deflection, angle, "worker"))
        return Part.meshDigest(here), Part.meshDigest(there)

    def testAHelpersMeshIsTheMeshMadeHere(self):
        twice = shapes()["twice"].childShapes()
        self.assertTrue(twice[0].isPartner(twice[1]))  # one solid, two places
        before = Part.meshServiceInfo()
        for name, shape in shapes().items():
            here, there = self.meshBoth(shape, [(0.1, 0.5)])
            self.assertEqual(here, there, name)
            self.assertFalse(here.startswith("faces 0 "), name)
        after = Part.meshServiceInfo()
        self.assertEqual(after["Worker"] - before["Worker"], len(shapes()))
        self.assertEqual(after["Died"], before["Died"])
        self.assertEqual(after["FellBack"], before["FellBack"])

    def testAMeshMadeAgainOverOneItHolds(self):
        # finer, then coarser over it (the quality decrease a view allows),
        # each over the mesh the step before left
        steps = [(0.5, 0.5), (0.05, 0.3), (1.0, 0.8)]
        for name in ("sphere", "fused", "twice"):
            here, there = self.meshBoth(shapes()[name], steps)
            self.assertEqual(here, there, name)
        # a mesh made here, then one asked of a helper over it
        shape = shapes()["fused"]
        here = shape.copy()
        there = shape.copy()
        for s in (here, there):
            Part.meshShape(s, 0.5, 0.5, "here")
        Part.meshShape(here, 0.05, 0.3, "here")
        Part.meshShape(there, 0.05, 0.3, "worker")
        self.assertEqual(Part.meshDigest(here), Part.meshDigest(there))

    def testTheSettingSendsItToAHelper(self):
        self.params.SetBool("MeshInWorker", True)
        before = Part.meshServiceInfo()
        shape = Part.makeBox(3, 4, 5)
        self.assertTrue(Part.meshShape(shape, 0.1, 0.5))
        after = Part.meshServiceInfo()
        self.assertEqual(after["Worker"], before["Worker"] + 1)
        self.params.SetBool("MeshInWorker", False)
        self.assertTrue(Part.meshShape(Part.makeBox(3, 4, 5), 0.1, 0.5))
        self.assertEqual(Part.meshServiceInfo()["Here"], after["Here"] + 1)

    def testAShapeThatKillsItsHelperCostsOnlyTheHelper(self):
        before = Part.meshServiceInfo()
        killer = Part.makeBox(1, 2, 3)
        self.assertFalse(Part.meshShape(killer, 0.1, 0.5, "worker", True))
        self.assertTrue(Part.meshDigest(killer).startswith("faces 0 "))
        info = Part.meshServiceInfo()
        self.assertEqual(info["Died"], before["Died"] + 1)
        # not sent again, not meshed here either
        self.assertFalse(Part.meshShape(killer, 0.1, 0.5, "worker"))
        self.assertTrue(Part.meshDigest(killer).startswith("faces 0 "))
        self.assertEqual(Part.meshServiceInfo()["Refused"], info["Refused"] + 1)
        # the next shape meshed by a helper started for it
        shape = Part.makeCylinder(2, 3)
        self.assertTrue(Part.meshShape(shape, 0.1, 0.5, "worker"))
        self.assertFalse(Part.meshDigest(shape).startswith("faces 0 "))
        self.assertGreater(Part.meshServiceInfo()["Spawned"], info["Spawned"])

    def setting(self, kind, name, value):
        """A setting of Preferences/Workers for this test only"""
        getattr(self.params, "Set" + kind)(name, value)
        self.addCleanup(getattr(self.params, "Rem" + kind), name)

    def testTheHelpersAreOneAThreadByDefault(self):
        """As many helpers as the machine runs threads at once, 2 at least:
        a load's pre-mesh posts to that many threads, and each blocks in
        the call for a helper (docs/WorkerProcess.md sec 71)"""
        import os

        if "ServiceWorkers" in self.params.GetInts():
            self.addCleanup(self.params.SetInt, "ServiceWorkers",
                            self.params.GetInt("ServiceWorkers"))
        self.params.RemInt("ServiceWorkers")
        self.assertEqual(Part.meshServiceInfo()["Limit"], max(2, os.cpu_count()))
        self.setting("Int", "ServiceWorkers", 3)
        self.assertEqual(Part.meshServiceInfo()["Limit"], 3)

    def testAHelperPastThoseKeptEndsOnceIdle(self):
        """One past ServiceWorkersKept ends its process once idle for
        ServiceIdleSeconds, and the next call starts one again"""
        import time

        self.setting("Int", "ServiceWorkersKept", 0)
        self.setting("Float", "ServiceIdleSeconds", 0.3)
        self.assertTrue(Part.meshShape(Part.makeBox(1, 2, 3), 0.1, 0.5, "worker"))
        before = Part.meshServiceInfo()
        end = time.time() + 20
        while Part.meshServiceInfo()["Helpers"] and time.time() < end:
            time.sleep(0.05)
        info = Part.meshServiceInfo()
        self.assertEqual(info["Helpers"], 0, info)
        self.assertGreater(info["IdleEnded"], before["IdleEnded"], info)
        shape = Part.makeCylinder(2, 3)
        self.assertTrue(Part.meshShape(shape, 0.1, 0.5, "worker"))
        self.assertFalse(Part.meshDigest(shape).startswith("faces 0 "))
        self.assertGreater(Part.meshServiceInfo()["Spawned"], info["Spawned"])

    def testAMessageTooBigForAHelperIsMeshedHere(self):
        """A request, or a helper's reply, past what a message carries is
        not sent: the call ends saying why, the shape is meshed here, and
        the helper goes on (docs/WorkerProcess.md secs 77, 80). The size is
        lowered by the test's seam, FC_WORKER_MESSAGE_MOST, which a helper
        reads from its environment: so the helpers standing end first, and
        the one the call starts has it"""
        import os
        import time

        self.setting("Int", "ServiceWorkersKept", 0)
        self.setting("Float", "ServiceIdleSeconds", 0.3)

        def most(n):
            # a call under these settings first: a helper left idle before
            # them ends by the settings it went idle under
            os.environ.pop("FC_WORKER_MESSAGE_MOST", None)
            self.assertTrue(Part.meshShape(Part.makeBox(1, 2, 3), 0.1, 0.5, "worker"))
            os.environ["FC_WORKER_MESSAGE_MOST"] = str(n)
            end = time.time() + 20
            while Part.meshServiceInfo()["Helpers"] and time.time() < end:
                time.sleep(0.05)
            self.assertEqual(Part.meshServiceInfo()["Helpers"], 0)

        self.addCleanup(os.environ.pop, "FC_WORKER_MESSAGE_MOST", None)

        def meshed(shape, deflection):
            here = shape.copy()
            there = shape.copy()
            self.assertTrue(Part.meshShape(here, deflection, 0.5, "here"))
            self.assertTrue(Part.meshShape(there, deflection, 0.5, "worker"))
            return Part.meshDigest(here), Part.meshDigest(there)

        # the request: a box's is some 4.5 KB
        most(2000)
        before = Part.meshServiceInfo()
        here, there = meshed(Part.makeBox(10, 20, 30), 0.1)
        info = Part.meshServiceInfo()
        self.assertEqual(here, there)
        self.assertEqual(info["FellBack"], before["FellBack"] + 1, info)
        self.assertEqual(info["Died"], before["Died"], info)
        self.assertEqual(info["Worker"], before["Worker"], info)
        # the reply: a fine sphere's is some 220 KB, its request 1 KB
        most(64 * 1024)
        before = Part.meshServiceInfo()
        here, there = meshed(Part.makeSphere(7), 0.01)
        info = Part.meshServiceInfo()
        self.assertEqual(here, there)
        self.assertEqual(info["FellBack"], before["FellBack"] + 1, info)
        self.assertEqual(info["Died"], before["Died"], info)
        self.assertEqual(info["Spawned"], before["Spawned"] + 1, info)
        # the helper that refused it meshes the next
        pids = info["Pids"]
        shape = Part.makeBox(1, 2, 3)
        self.assertTrue(Part.meshShape(shape, 0.1, 0.5, "worker"))
        after = Part.meshServiceInfo()
        self.assertEqual(after["Worker"], info["Worker"] + 1, after)
        self.assertEqual(after["Spawned"], info["Spawned"], after)
        self.assertEqual(after["Pids"], pids)

    def testAHelpersMeetingSocketGoesOnceItIsMet(self):
        # The socket a helper is met at is a file in the temporary folder on
        # macOS and Linux (a named pipe on Windows, nothing to find). It goes
        # as soon as the helper is met and known, so a crash or a kill later
        # leaves none -- and the connection made through it outlives it: every
        # call below is answered over a connection whose server has closed.
        import glob
        import os
        import tempfile

        mine = os.path.join(tempfile.gettempdir(), "thundereal-service-%d-*" % os.getpid())
        self.assertTrue(Part.meshShape(Part.makeBox(1, 2, 3), 0.1, 0.5, "worker"))
        met = Part.meshServiceInfo()
        self.assertGreaterEqual(met["Helpers"], 1, met)
        self.assertEqual(glob.glob(mine), [], met)
        for i in range(4):
            self.assertTrue(Part.meshShape(Part.makeCylinder(2, 3 + i), 0.1, 0.5, "worker"))
        after = Part.meshServiceInfo()
        self.assertEqual(after["Worker"], met["Worker"] + 4, after)
        self.assertEqual(after["Died"], met["Died"], after)
        # the helpers that answered the first call are still the ones running
        self.assertTrue(set(met["Pids"]) <= set(after["Pids"]), (met["Pids"], after["Pids"]))
        self.assertEqual(glob.glob(mine), [], after)
