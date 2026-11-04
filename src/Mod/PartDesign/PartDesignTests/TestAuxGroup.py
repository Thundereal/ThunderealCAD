# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
# *                                                                         *
# *   This file is part of FreeCAD.                                         *
# *                                                                         *
# *   FreeCAD is free software: you can redistribute it and/or modify it    *
# *   under the terms of the GNU Lesser General Public License as           *
# *   published by the Free Software Foundation, either version 2.1 of the  *
# *   License, or (at your option) any later version.                       *
# *                                                                         *
# *   FreeCAD is distributed in the hope that it will be useful, but        *
# *   WITHOUT ANY WARRANTY; without even the implied warranty of            *
# *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU     *
# *   Lesser General Public License for more details.                       *
# *                                                                         *
# *   You should have received a copy of the GNU Lesser General Public      *
# *   License along with FreeCAD. If not, see                               *
# *   <https://www.gnu.org/licenses/>.                                      *
# *                                                                         *
# ***************************************************************************

"""PartDesign::AuxGroup on the generic App::AuxGroup.

The Body's Sketches, Datums and Misc groups list the body's children of
their role; a group the user adds lists what the user put in it. A group
reads its role from Role, or from its name in a file written before Role.
"""

import os
import tempfile
import unittest

import FreeCAD


class TestAuxGroup(unittest.TestCase):
    def setUp(self):
        self.doc = FreeCAD.newDocument("PartDesignTestAuxGroup")
        self.body = self.doc.addObject("PartDesign::Body", "Body")
        self.sketch = self.body.newObject("Sketcher::SketchObject", "Sketch")
        self.plane = self.body.newObject("PartDesign::Plane", "DatumPlane")
        self.box = self.body.newObject("PartDesign::AdditiveBox", "Box")
        self.binder = self.body.newObject("Part::SubShapeBinder", "Binder")

    def tearDown(self):
        FreeCAD.closeDocument(self.doc.Name)

    def addGroup(self, name, role=None):
        """Add a group as the Body's context menu does"""
        group = self.doc.addObject("PartDesign::AuxGroup", name)
        if role is not None:
            group.Role = role
        self.body.Group = [group] + self.body.Group
        group._Body = self.body
        return group

    def addRoleGroups(self):
        return (
            self.addGroup("Sketches", "Sketches"),
            self.addGroup("Datums", "Datums"),
            self.addGroup("Misc", "Misc"),
        )

    def testIsAnAppAuxGroup(self):
        group = self.addGroup("Group")
        self.assertTrue(group.isDerivedFrom("App::AuxGroup"))
        self.assertTrue(group.isDerivedFrom("PartDesign::AuxGroup"))

    def testRolesFilterTheBodysChildren(self):
        sketches, datums, misc = self.addRoleGroups()
        self.assertEqual(sketches.Group, [self.sketch])
        self.assertEqual(datums.Group, [self.plane])
        self.assertEqual(misc.Group, [self.binder])

    def testANewChildJoinsItsGroup(self):
        sketches, _, _ = self.addRoleGroups()
        sketch2 = self.body.newObject("Sketcher::SketchObject", "Sketch001")
        self.assertEqual(sketches.Group, [self.sketch, sketch2])

    def testRoleFromTheNameOfAnOlderFile(self):
        group = self.addGroup("Datums001")
        self.assertEqual(group.Role, "")
        self.assertEqual(group.Group, [self.plane])

    def testRoleWinsOverTheName(self):
        group = self.addGroup("Sketches001", "Datums")
        self.assertEqual(group.Group, [self.plane])

    def testCuratedGroupKeepsTheBodysOrder(self):
        group = self.addGroup("Group")
        self.assertEqual(group.Group, [])
        group.Group = [self.plane, self.sketch]
        # Any change of the body's children refreshes the group
        self.body.newObject("Sketcher::SketchObject", "Sketch001")
        self.assertEqual(group.Group, [self.sketch, self.plane])

    def testRemovedFromTheBodyLosesItsOwner(self):
        sketches, _, _ = self.addRoleGroups()
        self.body.Group = [o for o in self.body.Group if o != sketches]
        self.assertIsNone(sketches._Body)
        self.assertEqual(sketches.Group, [])

    def testVisibilityOfAGroupIsItsChildren(self):
        sketches, _, _ = self.addRoleGroups()
        self.body.setElementVisible(sketches.Name, False)
        self.assertFalse(self.sketch.Visibility)
        self.assertEqual(self.body.isElementVisible(sketches.Name), 0)
        self.body.setElementVisible(sketches.Name, True)
        self.assertTrue(self.sketch.Visibility)
        self.assertEqual(self.body.isElementVisible(sketches.Name), 1)

    def testSaveAndRestore(self):
        self.addRoleGroups()
        old = self.addGroup("Sketches001")
        path = os.path.join(tempfile.mkdtemp(), "AuxGroup.FCStd")
        self.doc.saveAs(path)
        name = self.doc.Name
        FreeCAD.closeDocument(name)
        self.doc = FreeCAD.openDocument(path)
        body = self.doc.getObject("Body")
        sketches = self.doc.getObject("Sketches")
        old = self.doc.getObject("Sketches001")
        self.assertEqual(sketches.Role, "Sketches")
        self.assertEqual(old.Role, "")
        self.assertEqual(sketches._Body, body)
        sketch = self.doc.getObject("Sketch")
        self.assertEqual(sketches.Group, [sketch])
        # Still kept up after the restore
        sketch2 = body.newObject("Sketcher::SketchObject", "Sketch001")
        self.assertEqual(sketches.Group, [sketch, sketch2])
        self.assertEqual(old.Group, [sketch, sketch2])
