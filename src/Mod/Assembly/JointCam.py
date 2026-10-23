# SPDX-License-Identifier: LGPL-2.1-or-later
# /**************************************************************************
#                                                                           *
#    Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
#                                                                           *
#    This file is part of FreeCAD.                                          *
#                                                                           *
#    FreeCAD is free software: you can redistribute it and/or modify it     *
#    under the terms of the GNU Lesser General Public License as            *
#    published by the Free Software Foundation, either version 2.1 of the   *
#    License, or (at your option) any later version.                        *
#                                                                           *
#    FreeCAD is distributed in the hope that it will be useful, but         *
#    WITHOUT ANY WARRANTY; without even the implied warranty of             *
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
#    Lesser General Public License for more details.                        *
#                                                                           *
#    You should have received a copy of the GNU Lesser General Public       *
#    License along with FreeCAD. If not, see                                *
#    <https://www.gnu.org/licenses/>.                                       *
#                                                                           *
# **************************************************************************/

"""Not upstream: the Cam joint's specifics (docs/AssemblySolverInterface.md sec 6.18).

A Cam joint is the last entry of JointObject.JointTypes. Its Reference1 is the
cam -- an edge of the cam part, any curve -- and its Reference2 the follower --
a vertex (a point follower) or a circular edge (a roller, its centre held on
the cam curve offset by its radius). The solver side is App/CamJoint.cpp; the
joint task asks this module after each pick, so that a pick that cannot make
a cam is not taken, with the reason.
"""

import FreeCAD as App

translate = App.Qt.translate


def pickRefusal(joint, refs):
    """Why the picks (one or two references) cannot make a cam; "" when they can.

    The picks are set on the joint, as setJointConnectors sets them, without
    solving; the rule and the reason are the C++ ones (AssemblyApp.getCamRefusal).
    """
    if not refs:
        return ""
    import AssemblyApp

    joint.Reference1 = refs[0]
    joint.Reference2 = refs[1] if len(refs) > 1 else None
    return AssemblyApp.getCamRefusal(joint)


def refusalMessage(reason):
    """The task panel's message for a refused pick."""
    return translate(
        "Assembly",
        "A cam between these elements is not supported: {}. Select other elements.",
    ).format(reason)
