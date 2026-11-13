# SPDX-License-Identifier: LGPL-2.1-or-later

"""A reference that resolves is checked against the element it was on.

A reference goes by its element's name. A name that holds a place -- the
second of two results, the first of several -- can be another element's
after a recompute, and the reference then moves with nothing said
(docs/TopoNamingEnhance.md sec 9). When the object referred to changes its
shape, the element each reference was on is looked for in the new shape by
its geometry. Where it is still there, unchanged, as one element, and that
is not the element the name finds, the object holding the reference says so:
DocumentObject.getReferencesElsewhere(). The reference keeps going by its
name; a panel that shows it offers the element instead.

The shapes here are copies of one box, their edges named after the box's by
a history given by hand, so that two edges can change names with nothing
else changing.
"""

import os
import tempfile
import unittest

import FreeCAD
import Part


class TestReferenceGuard(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestReferenceGuard")
        self.Box = Part.makeBox(10, 20, 30)
        # each edge of the box an input of its own, under its own tag
        self.Sources = []
        for i, edge in enumerate(self.Box.Edges):
            edge.Tag = 100 + i
            self.Sources.append(edge)

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def named(self, order=None, box=None):
        """A copy of the box, edge order[i] named after the box's edge i"""
        shape = (box or self.Box).copy()
        order = order or list(range(len(self.Sources)))
        history = [(self.Sources[i], [shape.Edges[j]]) for i, j in enumerate(order)]
        shape.mapShapes([], history)
        return shape

    def swapped(self):
        order = list(range(len(self.Sources)))
        order[0], order[1] = order[1], order[0]
        return order

    def referTo(self, shape, element="Edge1"):
        feature = self.Doc.addObject("Part::Feature", "Feature")
        feature.Shape = shape
        holder = self.Doc.addObject("App::FeaturePython", "Holder")
        holder.addProperty("App::PropertyLinkSub", "Ref")
        holder.Ref = (feature, [element])
        self.Doc.recompute()
        return feature, holder

    def testANameGoneToAnotherElementIsSaid(self):
        # Arrange
        feature, holder = self.referTo(self.named())
        name = feature.Shape.getElementMappedName("Edge1")
        self.assertNotEqual(name, "Edge1")
        self.assertEqual(holder.getReferencesElsewhere(), [])

        # Act: the same shape made again, the names of its first two edges
        # the other way round
        feature.Shape = self.named(self.swapped())
        self.Doc.recompute()

        # Assert: the reference went with the name, as it always did -- and
        # it is said where the edge it was on is
        self.assertEqual(feature.Shape.getElementMappedName("Edge2"), name)
        self.assertEqual(holder.Ref, (feature, ["Edge2"]))
        self.assertEqual(holder.getReferencesElsewhere(), [("Ref", feature, "Edge2", "Edge1")])

    def testNothingIsSaidWhereTheElementChanged(self):
        # Arrange
        feature, holder = self.referTo(self.named())

        # Act: the names the other way round, on a box of another size: the
        # edge the reference was on is nowhere, and the name is all there is
        feature.Shape = self.named(self.swapped(), Part.makeBox(11, 21, 31))
        self.Doc.recompute()

        # Assert
        self.assertEqual(holder.Ref, (feature, ["Edge2"]))
        self.assertEqual(holder.getReferencesElsewhere(), [])

    def testNothingIsSaidWhereNothingMoved(self):
        # Arrange
        feature, holder = self.referTo(self.named())

        # Act: made again as it is
        feature.Shape = self.named()
        self.Doc.recompute()

        # Assert
        self.assertEqual(holder.Ref, (feature, ["Edge1"]))
        self.assertEqual(holder.getReferencesElsewhere(), [])

    def testItIsKeptThroughASaveAndUntilTheReferenceIsSet(self):
        # Arrange
        feature, holder = self.referTo(self.named())
        feature.Shape = self.named(self.swapped())
        self.Doc.recompute()
        said = [("Ref", "Feature", "Edge2", "Edge1")]

        def told(doc):
            return [
                (prop, obj.Name, element, elsewhere)
                for prop, obj, element, elsewhere in doc.Holder.getReferencesElsewhere()
            ]

        self.assertEqual(told(self.Doc), said)

        # Act: saved and opened again
        path = os.path.join(tempfile.mkdtemp(), "guard.FCStd")
        self.Doc.saveAs(path)
        FreeCAD.closeDocument(self.Doc.Name)
        self.Doc = FreeCAD.openDocument(path)

        # Assert
        self.assertEqual(told(self.Doc), said)

        # Act: another recompute that moves nothing
        self.Doc.Feature.touch()
        self.Doc.recompute()

        # Assert: still said
        self.assertEqual(told(self.Doc), said)

        # Act: the reference set to the edge it was on
        self.Doc.Holder.Ref = (self.Doc.Feature, ["Edge1"])
        self.Doc.recompute()

        # Assert
        self.assertEqual(told(self.Doc), [])

    def testAListsReferenceIsSaidByItself(self):
        # Arrange: two references in a list, to the edges that change names
        # and to one that does not
        feature = self.Doc.addObject("Part::Feature", "Feature")
        feature.Shape = self.named()
        holder = self.Doc.addObject("App::FeaturePython", "Holder")
        holder.addProperty("App::PropertyLinkSubList", "Refs")
        holder.Refs = [(feature, ["Edge1", "Edge5"])]
        self.Doc.recompute()

        # Act
        feature.Shape = self.named(self.swapped())
        self.Doc.recompute()

        # Assert
        self.assertEqual(holder.getReferencesElsewhere(), [("Refs", feature, "Edge2", "Edge1")])
