# SPDX-License-Identifier: LGPL-2.1-or-later

# Tab and slot (docs/SheetMetal.md sec 4.8, M5, ruled 2026-11-06): tabs on
# the end of one wall and matching slots cut through another part it stands
# on, with a clearance, so laser-cut parts locate themselves for welding.
# PartDesign::SheetMetalLocatingTabs, in the tabbed part's body, puts Count
# tabs Width wide and Length out on a wall's end face, spread between end
# margins; PartDesign::SheetMetalSlots, in the other part's body, cuts the
# tabs' shapes, grown by Clearance, through it, and follows them.
#
# A base plate 60 x 40, 1 thick (z 0..1), in its body; an upright wall 40
# long (x 10..50) and 20 high standing on it (z 1..21), in a body of its
# own, about y 20.

import FreeCAD
import Part
import unittest

V = FreeCAD.Vector
T = 1.0


def faceAt(feature, p, normal):
    """The plane face of a feature's shape through p, facing normal"""
    vertex = Part.Vertex(V(*p))
    for i, f in enumerate(feature.Shape.Faces):
        if not isinstance(f.Surface, Part.Plane) or f.distToShape(vertex)[0] > 1e-7:
            continue
        if f.normalAt(0, 0).isEqual(V(*normal), 1e-7):
            return "Face%d" % (i + 1)
    raise ValueError("no face at %s" % (p,))


class TestSheetMetalTabSlot(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalTabSlot")
        self.Base = self.Doc.addObject("PartDesign::Body", "Base")
        self.Plate = self.wall(self.Base, "Plate", [(0, 0), (60, 0), (60, 40), (0, 40)])
        self.Upright = self.Doc.addObject("PartDesign::Body", "Upright")
        # on the XZ plane at y 20, its sketch's Y up
        self.Leg = self.wall(
            self.Upright,
            "Leg",
            [(10, 1), (50, 1), (50, 21), (10, 21)],
            FreeCAD.Placement(V(0, 20, 0), FreeCAD.Rotation(V(1, 0, 0), 90)),
        )
        box = self.Leg.Shape.BoundBox
        self.Y0, self.Y1 = box.YMin, box.YMax
        self.assertAlmostEqual(self.Y1 - self.Y0, T)
        self.assertAlmostEqual(box.ZMin, 1)

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def wall(self, body, name, pts, placement=None):
        sketch = body.newObject("Sketcher::SketchObject", name + "Sketch")
        if placement:
            sketch.Placement = placement
        for i in range(len(pts)):
            a, b = pts[i], pts[(i + 1) % len(pts)]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        wall = body.newObject("PartDesign::SheetMetalBaseWall", name)
        wall.Profile = sketch
        self.Doc.recompute()
        return wall

    def tabs(self, **props):
        """Tabs on the leg's foot, its end face at z 1 facing down"""
        tabs = self.Upright.newObject("PartDesign::SheetMetalLocatingTabs", "Tabs")
        mid = (self.Y0 + self.Y1) / 2
        tabs.Face = (self.Leg, [faceAt(self.Leg, (30, mid, 1), (0, 0, -1))])
        sizes = dict(Count=2, Width=6, Length=1, Margin=5)
        sizes.update(props)
        for k, v in sizes.items():
            setattr(tabs, k, v)
        self.Doc.recompute()
        return tabs

    def slots(self, tabs, **props):
        slots = self.Base.newObject("PartDesign::SheetMetalSlots", "Slots")
        slots.Tabs = tabs
        slots.Clearance = props.pop("Clearance", 0.1)
        for k, v in props.items():
            setattr(slots, k, v)
        self.Doc.recompute()
        return slots

    def flat(self, feature):
        flat = feature.getSubObject("Flat")
        self.assertIsNotNone(flat)
        return flat

    def testTabs(self):
        """Two tabs 6 wide and 1 out of the leg's foot, 5 in from its ends:
        x 15..21 and 39..45, z 0..1, the leg's thickness; the wall's, lying
        flat with it"""
        tabs = self.tabs()
        self.assertNotIn("Invalid", tabs.State, tabs.getStatusString())
        shape = tabs.Shape
        self.assertTrue(shape.isValid())
        self.assertEqual(len(shape.Solids), 1)
        self.assertAlmostEqual(shape.Volume, self.Leg.Shape.Volume + 2 * 6 * 1 * T, places=6)
        mid = (self.Y0 + self.Y1) / 2
        for x in (15.5, 20.5, 39.5, 44.5):
            self.assertTrue(shape.isInside(V(x, mid, 0.5), 1e-7, True), x)
        for x in (14.5, 30, 45.5):
            self.assertFalse(shape.isInside(V(x, mid, 0.5), 1e-7, True), x)
        owners = [tabs.sheetElementOf("Face%d" % (i + 1)) for i in range(len(shape.Faces))]
        self.assertEqual(set(owners), {"Wall1"})
        self.assertAlmostEqual(self.flat(tabs).Volume, shape.Volume, places=6)

    def testOneTab(self):
        """Count 1: in the middle"""
        tabs = self.tabs(Count=1)
        self.assertNotIn("Invalid", tabs.State, tabs.getStatusString())
        mid = (self.Y0 + self.Y1) / 2
        self.assertTrue(tabs.Shape.isInside(V(30, mid, 0.5), 1e-7, True))
        self.assertFalse(tabs.Shape.isInside(V(33.5, mid, 0.5), 1e-7, True))

    def testSlots(self):
        """Slots through the plate where the tabs go, grown by the
        clearance: 6.2 by 1.2 each; the tabs fit them, touching nothing;
        the plate lies flat with its slots"""
        tabs = self.tabs()
        slots = self.slots(tabs)
        self.assertNotIn("Invalid", slots.State, slots.getStatusString())
        shape = slots.Shape
        self.assertTrue(shape.isValid())
        self.assertEqual(len(shape.Solids), 1)
        self.assertAlmostEqual(shape.Volume, 60 * 40 * T - 2 * 6.2 * 1.2 * T, places=6)
        mid = (self.Y0 + self.Y1) / 2
        self.assertFalse(shape.isInside(V(18, mid, 0.5), 1e-7, True))
        self.assertFalse(shape.isInside(V(14.95, mid, 0.5), 1e-7, True))
        self.assertTrue(shape.isInside(V(14.8, mid, 0.5), 1e-7, True))
        self.assertTrue(shape.isInside(V(18, self.Y1 + 0.2, 0.5), 1e-7, True))
        self.assertTrue(shape.isInside(V(30, mid, 0.5), 1e-7, True))
        # the tabs, in the plate's frame, go through it touching nothing
        self.assertLess(tabs.Shape.common(shape).Volume, 1e-9)
        self.assertAlmostEqual(self.flat(slots).Volume, shape.Volume, places=6)

    def testSlotsFollow(self):
        """The tabs changed, the slots follow"""
        tabs = self.tabs()
        slots = self.slots(tabs)
        tabs.Count = 3
        self.Doc.recompute()
        self.assertNotIn("Invalid", slots.State, slots.getStatusString())
        self.assertAlmostEqual(slots.Shape.Volume, 60 * 40 * T - 3 * 6.2 * 1.2 * T, places=6)
        self.assertLess(tabs.Shape.common(slots.Shape).Volume, 1e-9)

    def testSlotsMoved(self):
        """The upright's body moved along the plate: the slots go with it"""
        tabs = self.tabs()
        slots = self.slots(tabs)
        self.Upright.Placement = FreeCAD.Placement(V(5, 0, 0), FreeCAD.Rotation())
        self.Doc.recompute()
        self.assertNotIn("Invalid", slots.State, slots.getStatusString())
        mid = (self.Y0 + self.Y1) / 2
        self.assertFalse(slots.Shape.isInside(V(23, mid, 0.5), 1e-7, True))
        self.assertTrue(slots.Shape.isInside(V(18, mid, 0.5), 1e-7, True))

    def drop(self, body, feature):
        body.removeObject(feature)
        self.Doc.removeObject(feature.Name)
        self.Doc.recompute()

    def testRefused(self):
        """Tabs on a skin, not an end face; more than fit between the
        margins; none; slots of no tabs, or of tabs that miss the part"""
        tabs = self.tabs()
        tabs.Face = (self.Leg, [faceAt(self.Leg, (30, self.Y1, 10), (0, 1, 0))])
        self.Doc.recompute()
        self.assertIn("Invalid", tabs.State)
        self.drop(self.Upright, tabs)
        for props in (dict(Count=6), dict(Width=0)):
            with self.subTest(**props):
                tabs = self.tabs(**props)
                self.assertIn("Invalid", tabs.State)
                self.drop(self.Upright, tabs)
        # none is held at one
        tabs = self.tabs(Count=0)
        self.assertEqual(tabs.Count, 1)
        self.drop(self.Upright, tabs)
        slots = self.slots(None)
        self.assertIn("Invalid", slots.State)
        self.drop(self.Base, slots)
        tabs = self.tabs()
        self.Upright.Placement = FreeCAD.Placement(V(0, 0, 30), FreeCAD.Rotation())
        self.Doc.recompute()
        slots = self.slots(tabs)
        self.assertIn("Invalid", slots.State)

if __name__ == "__main__":
    unittest.main()
