# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from Base.Metadata import constmethod, export
from Base.Vector import Vector

from Assembly.FramedItem import FramedItem

@export(
    Include="Mod/Assembly/App/Physics.h",
    Namespace="Assembly",
    FatherInclude="Mod/Assembly/App/FramedItemPy.h",
    FatherNamespace="Assembly",
)
class AppliedLoad(FramedItem):
    """
    A force or a torque applied to a component in dynamic and static runs, at
    a free frame or an element's frame through an element binding the load
    owns (docs/AssemblyDynamics.md).

    Author: Zheng Lei (realthunder.dev@gmail.com)
    License: LGPL-2.1-or-later
    """

    @constmethod
    def getDirectionNow(self) -> Vector:
        """
        The direction of the force, or the axis of the torque, in the
        assembly's coordinate system now: a unit vector, or a zero one when
        Direction is zero.
        """
        ...
