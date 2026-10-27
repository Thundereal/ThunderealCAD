# SPDX-License-Identifier: LGPL-2.1-or-later
"""The interference check (docs/AssemblyInterference.md): AssemblyObject.
findInterferences in both modes, on a hand-made assembly whose answers are
known -- an overlap of 200 mm^3, a box inside another, two bars crossing,
faces in contact, a pin snug in its hole, a gap of 0.05 mm.
"""

import unittest

import FreeCAD as App
import Part


class TestInterference(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("TestInterference")
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")

        def box(name, l, w, h, x, y, z):
            b = self.assembly.newObject("Part::Box", name)
            b.Length, b.Width, b.Height = l, w, h
            b.Placement.Base = App.Vector(x, y, z)
            return b

        self.A = box("A", 10, 10, 10, 0, 0, 0)
        self.B = box("B", 10, 10, 10, 8, 0, 0)  # into A by 2: 200 mm^3
        self.C = box("C", 10, 10, 10, 0, 10, 0)  # on A's face, on B's
        self.D = box("D", 10, 10, 10, 100, 0, 0)  # far from all
        self.Big = box("Big", 30, 30, 30, 200, 0, 0)
        self.Small = box("Small", 2, 2, 2, 210, 10, 10)  # inside Big: 8 mm^3
        self.Plate = self.assembly.newObject("Part::Feature", "Plate")
        self.Plate.Shape = Part.makeBox(20, 20, 4, App.Vector(300, 0, 0)).cut(
            Part.makeCylinder(3, 4, App.Vector(310, 10, 0))
        )
        self.Pin = self.assembly.newObject("Part::Cylinder", "Pin")  # snug in the hole
        self.Pin.Radius, self.Pin.Height = 3, 10
        self.Pin.Placement.Base = App.Vector(310, 10, -3)
        # Two bars crossing: 8 mm^3 in common, no vertex of either in the other
        self.Bar1 = box("Bar1", 40, 2, 2, 400, 19, 0)
        self.Bar2 = box("Bar2", 2, 40, 2, 419, 0, 0)
        self.Gap1 = box("Gap1", 10, 10, 10, 500, 0, 0)
        self.Gap2 = box("Gap2", 10, 10, 10, 510.05, 0, 0)  # 0.05 mm away
        self.doc.recompute()

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def pairs(self, results, kind):
        return sorted(
            tuple(sorted((r["Object1"].Name, r["Object2"].Name)))
            for r in results
            if r["Kind"] == kind
        )

    def test_exact(self):
        """The volumes in common, and the solid of it; contacts when asked."""
        results = self.assembly.findInterferences(mode="Exact", tolerance=0.01)
        self.assertEqual(
            self.pairs(results, "Interference"),
            [("A", "B"), ("Bar1", "Bar2"), ("Big", "Small")],
        )
        self.assertEqual(self.pairs(results, "Contact"), [])
        volumes = {tuple(sorted((r["Object1"].Name, r["Object2"].Name))): r for r in results}
        self.assertAlmostEqual(volumes[("A", "B")]["Volume"], 200.0, places=6)
        self.assertAlmostEqual(volumes[("Big", "Small")]["Volume"], 8.0, places=6)
        self.assertAlmostEqual(volumes[("Bar1", "Bar2")]["Volume"], 8.0, places=6)
        overlap = volumes[("A", "B")]["Overlap"]
        self.assertIsNotNone(overlap)
        self.assertAlmostEqual(overlap.Volume, 200.0, places=6)
        self.assertAlmostEqual(overlap.BoundBox.XMin, 8.0, places=6)

        results = self.assembly.findInterferences(mode="Exact", tolerance=0.01, contacts=True)
        self.assertEqual(
            self.pairs(results, "Contact"), [("A", "C"), ("B", "C"), ("Pin", "Plate")]
        )
        for r in results:
            if r["Kind"] == "Contact":
                self.assertLess(r["Distance"], 1e-6)

    def test_fast(self):
        """The same pairs as Exact, on meshes; Fast says that they overlap."""
        results = self.assembly.findInterferences(mode="Fast", tolerance=0.01, contacts=True)
        self.assertEqual(
            self.pairs(results, "Interference"),
            [("A", "B"), ("Bar1", "Bar2"), ("Big", "Small")],
        )
        self.assertEqual(
            self.pairs(results, "Contact"), [("A", "C"), ("B", "C"), ("Pin", "Plate")]
        )
        for r in results:
            self.assertEqual(r["Volume"], 0.0)
            self.assertIsNone(r["Overlap"])
            self.assertGreaterEqual(r["Tolerance"], 0.01)

    def test_tolerance(self):
        """A gap within the tolerance is a contact, beyond it nothing."""
        for mode in ("Exact", "Fast"):
            gap = [self.Gap1, self.Gap2]
            near = self.assembly.findInterferences(
                mode=mode, tolerance=0.1, contacts=True, objects=gap, deflection=0.001
            )
            self.assertEqual(self.pairs(near, "Contact"), [("Gap1", "Gap2")], mode)
            far = self.assembly.findInterferences(
                mode=mode, tolerance=0.01, contacts=True, objects=gap, deflection=0.001
            )
            self.assertEqual(far, [], mode)
        exact = self.assembly.findInterferences(
            mode="Exact", tolerance=0.1, contacts=True, objects=[self.Gap1, self.Gap2]
        )
        self.assertAlmostEqual(exact[0]["Distance"], 0.05, places=6)

    def test_objects(self):
        """Only the components given."""
        results = self.assembly.findInterferences(objects=[self.A, self.B, self.D])
        self.assertEqual(self.pairs(results, "Interference"), [("A", "B")])

    def test_link(self):
        """A link is a component, its shape where the link puts it."""
        link = self.assembly.newObject("App::Link", "Copy")
        link.LinkedObject = self.D
        # The link's placement in place of D's: from x = 105, on D by 5
        link.Placement.Base = App.Vector(105, 0, 0)
        self.doc.recompute()
        results = self.assembly.findInterferences(objects=[self.D, link])
        self.assertEqual(len(results), 1)
        self.assertAlmostEqual(results[0]["Volume"], 500.0, places=6)
        results = self.assembly.findInterferences(mode="Fast", objects=[self.D, link])
        self.assertEqual(self.pairs(results, "Interference"), [("Copy", "D")])

    def test_bad_mode(self):
        with self.assertRaises(ValueError):
            self.assembly.findInterferences(mode="Approximate")
