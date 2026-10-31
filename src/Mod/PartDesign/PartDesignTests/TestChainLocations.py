# SPDX-License-Identifier: LGPL-2.1-or-later

# A chain of features does not stack locations on its shape
# (docs/SheetMetal.md sec 4.4, ruled 2026-10-31). A feature that moves its
# base into its own frame and has its placement applied to its result made a
# new location for each, so the two never cancelled, and every feature down a
# chain put two more on the shape: 64 additive boxes took 17 s to rebuild
# where 64 pads making the same part are fast. Counted here as the BREP's
# table of locations, which holds each one the shape carries.

import unittest

import FreeCAD
import Part

V = FreeCAD.Vector


def locationTable(shape):
    """The BREP's locations: each a datum, or a product of (index, power)"""
    lines = shape.exportBrepToString().splitlines()
    at = [i for i, line in enumerate(lines) if line.startswith("Locations")]
    if not at:
        return []
    i = at[0]
    count = int(lines[i].split()[1])
    table = []
    i += 1
    while len(table) < count:
        words = lines[i].split()
        if words[0] == "1":
            table.append(None)
            i += 4
        else:
            nums = [int(w) for w in words[1:]]
            while nums[-1] != 0:
                i += 1
                nums += [int(w) for w in lines[i].split()]
            table.append(list(zip(nums[0:-1:2], nums[1:-1:2])))
            i += 1
    return table


def locations(shape):
    return len(locationTable(shape))


def longestChain(shape):
    """How many datums the longest location is a product of"""
    table = locationTable(shape)
    memo = {}

    def length(index):
        if index not in memo:
            entry = table[index - 1]
            memo[index] = 1 if entry is None else sum(length(j) for j, _ in entry)
        return memo[index]

    return max((length(i + 1) for i in range(len(table))), default=0)


class TestChainLocations(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestChainLocations")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def boxes(self, n):
        plate = self.Body.newObject("PartDesign::AdditiveBox", "Plate")
        plate.Length, plate.Width, plate.Height = 300, 30, 2
        self.Doc.recompute()
        last = plate
        for k in range(n):
            box = self.Body.newObject("PartDesign::AdditiveBox", "Box")
            box.Length, box.Width, box.Height = 10, 2, 20
            box.Placement = FreeCAD.Placement(V(k * 15 + 15, 28, 2), FreeCAD.Rotation())
            self.Doc.recompute()
            self.assertNotIn("Invalid", box.State)
            last = box
        return last

    def testPrimitives(self):
        """Twice as many boxes, no longer a chain of locations: each placed
        box keeps its faces under a location of its own, but no face's is a
        product growing with the chain"""
        eight = longestChain(self.boxes(8).Shape)
        FreeCAD.closeDocument(self.Doc.Name)
        self.setUp()
        last = self.boxes(16)
        self.assertLessEqual(longestChain(last.Shape), max(eight, 3))
        self.assertAlmostEqual(last.Shape.Volume, 300 * 30 * 2 + 16 * 400)

    def testDressUps(self):
        """A chamfer on a chamfer on ..., no more locations for each"""
        box = self.Body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = box.Height = 20
        self.Doc.recompute()
        counts = []
        base = box
        for z in (20, 0):
            for y in (0, 20):
                edge = [
                    "Edge%d" % (i + 1)
                    for i, e in enumerate(base.Shape.Edges)
                    if all(abs(v.Point.z - z) < 1e-7 and abs(v.Point.y - y) < 1e-7 for v in e.Vertexes)
                ]
                chamfer = self.Body.newObject("PartDesign::Chamfer", "Chamfer")
                chamfer.Base = (base, edge)
                chamfer.Size = 1
                self.Doc.recompute()
                self.assertNotIn("Invalid", chamfer.State)
                counts.append(locations(chamfer.Shape))
                base = chamfer
        self.assertLessEqual(counts[-1], counts[0] + 1, counts)
        self.assertLessEqual(longestChain(base.Shape), 2)

    def testPlacedBody(self):
        """A placed body's chain neither stacks nor moves its shape"""
        self.Body.Placement = FreeCAD.Placement(V(5, 6, 7), FreeCAD.Rotation(V(0, 0, 1), 30))
        last = self.boxes(4)
        box = last.Shape.BoundBox
        plate = self.Body.Group[[o.Name for o in self.Body.Group].index("Plate")]
        self.assertAlmostEqual(last.Shape.Volume, 300 * 30 * 2 + 4 * 400)
        self.assertTrue(box.isValid())
        self.assertAlmostEqual(last.Shape.BoundBox.ZMax, plate.Shape.BoundBox.ZMax + 20, places=6)
