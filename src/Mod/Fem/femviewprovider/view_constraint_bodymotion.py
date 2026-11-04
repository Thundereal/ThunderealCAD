# SPDX-License-Identifier: LGPL-2.1-or-later
"""The view of a ConstraintBodyMotion: a body load, nothing to draw. Not
upstream."""

__title__ = "FreeCAD FEM constraint body motion ViewProvider for the document object"
__url__ = "https://www.freecad.org"

## @package view_constraint_bodymotion

from . import view_base_femconstraint


class VPConstraintBodyMotion(view_base_femconstraint.VPBaseFemConstraint):
    """
    A View Provider for the FemConstraintBodyMotion object
    """

    pass
