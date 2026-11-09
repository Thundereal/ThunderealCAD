# SPDX-License-Identifier: LGPL-2.1-or-later
"""The body loads of a part in motion: gravity less what moving takes, for a
static analysis of a part at an instant of its motion (the Assembly
workbench's loads to FEM, docs/AssemblyDynamics.md D4a). Not upstream.

Per unit of mass at r from ReferencePoint, the load is
g - a - alpha x r - omega x (omega x r): Gravity, LinearAcceleration the
reference point's, AngularVelocity omega and AngularAcceleration alpha, all
along the document's axes."""

__title__ = "FreeCAD FEM constraint body motion document object"
__url__ = "https://www.freecad.org"

## @package constraint_bodymotion

import FreeCAD

from . import base_fempythonobject

_PropHelper = base_fempythonobject._PropHelper


class ConstraintBodyMotion(base_fempythonobject.BaseFemPythonObject):
    """
    The ConstraintBodyMotion object
    """

    Type = "Fem::ConstraintBodyMotion"

    def __init__(self, obj):
        super().__init__(obj)

        for prop in self._get_properties():
            prop.add_to_object(obj)
        # Every element takes it: no references
        obj.setEditorMode("References", 2)

    def _get_properties(self):
        group = "Body Motion"
        return [
            _PropHelper(
                type="App::PropertyPosition",
                name="ReferencePoint",
                group=group,
                doc="The point whose acceleration LinearAcceleration is; the turn is about it",
                value=FreeCAD.Vector(),
            ),
            _PropHelper(
                type="App::PropertyVector",
                name="Gravity",
                group=group,
                doc="Gravity, mm/s^2",
                value=FreeCAD.Vector(),
            ),
            _PropHelper(
                type="App::PropertyVector",
                name="LinearAcceleration",
                group=group,
                doc="The reference point's acceleration, mm/s^2",
                value=FreeCAD.Vector(),
            ),
            _PropHelper(
                type="App::PropertyVector",
                name="AngularVelocity",
                group=group,
                doc="The part's turn, rad/s",
                value=FreeCAD.Vector(),
            ),
            _PropHelper(
                type="App::PropertyVector",
                name="AngularAcceleration",
                group=group,
                doc="The turn's rate, rad/s^2",
                value=FreeCAD.Vector(),
            ),
        ]
