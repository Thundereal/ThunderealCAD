# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from typing import Optional, Tuple

from Base.Metadata import constmethod, export
from Base.Placement import Placement
from Base.Vector import Vector

from App.DocumentObject import DocumentObject

@export(
    Include="Mod/Assembly/App/Physics.h",
    Namespace="Assembly",
)
class AppliedLoad(DocumentObject):
    """
    A force or a torque applied to a component in dynamic and static runs, at
    a free frame or an element's frame through an element binding the load
    owns (docs/AssemblyDynamics.md).

    Author: Zheng Lei (realthunder.dev@gmail.com)
    License: LGPL-2.1-or-later
    """

    @constmethod
    def getEnd(self) -> Tuple[Optional[DocumentObject], Placement]:
        """
        The component the load acts on (None: the ground) and the application
        frame in its coordinates.
        """
        ...

    @constmethod
    def getEndPlacement(self) -> Placement:
        """
        The application frame in the assembly's coordinate system now.
        """
        ...

    @constmethod
    def getDirectionNow(self) -> Vector:
        """
        The direction of the force, or the axis of the torque, in the
        assembly's coordinate system now: a unit vector, or a zero one when
        Direction is zero.
        """
        ...

    def setEnd(
        self,
        component: DocumentObject,
        subname: str,
        vertex: str = "",
        /,
    ) -> DocumentObject:
        """
        Put the application frame on an element, picked as a joint's side is:
        a direct child of the assembly, the element in it as a selection gives
        it, and optionally a vertex placing the origin on the element. The old
        element binding is deleted.

        Returns: the new element binding.
        """
        ...

    def clearEnd(self) -> None:
        """
        Make the application frame a free one where it is, deleting its
        element binding.
        """
        ...
