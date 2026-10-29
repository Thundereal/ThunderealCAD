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

"""Contact sets (docs/AssemblyDynamics.md sec 7, phase D2): the components
of one group touching those of another, or one another, in the Dynamic and
Static studies, by the impact law and friction of Assembly::ContactSet."""

import FreeCAD as App

translate = App.Qt.translate


def newContactSet(assembly, groupA, groupB=()):
    """A contact set in the assembly's Physics group: the components of
    groupA touching those of groupB, or, groupB empty, one another; ticked in
    every Dynamic and Static study of the assembly"""
    physics = assembly.getGroup("Physics", True)
    contact = physics.newObject("Assembly::ContactSet", "ContactSet")
    contact.GroupA = list(groupA)
    contact.GroupB = list(groupB)
    sims = assembly.getGroup("Simulations")
    for sim in sims.Group if sims else []:
        if getattr(sim, "AnalysisType", None) in ("Dynamic", "Static"):
            sim.Elements = list(sim.Elements) + [contact]
    return contact
