# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from typing import Optional, Tuple

from Base.Metadata import constmethod, export
from Base.Vector import Vector

from App.DocumentObject import DocumentObject

@export(
    Include="Mod/Assembly/App/Physics.h",
    Namespace="Assembly",
)
class SpringDamper(DocumentObject):
    """
    A translational spring and damper between two ends, each a free point or
    the origin of an element's frame through an element binding the spring
    owns (docs/AssemblyDynamics.md).

    Author: Zheng Lei (realthunder.dev@gmail.com)
    License: LGPL-2.1-or-later
    """

    @constmethod
    def getEnd(self, side: int, /) -> Tuple[Optional[DocumentObject], Vector]:
        """
        The component end 1 or 2 is on (None: the ground) and the end in its
        coordinates (the assembly's for the ground).
        """
        ...

    @constmethod
    def getEndPoint(self, side: int, /) -> Vector:
        """
        End 1 or 2 in the assembly's coordinate system now.
        """
        ...

    def setEnd(
        self,
        side: int,
        component: DocumentObject,
        subname: str,
        vertex: str = "",
        /,
    ) -> DocumentObject:
        """
        Put end 1 or 2 on an element, picked as a joint's side is: a direct
        child of the assembly, the element in it as a selection gives it, and
        optionally a vertex placing the end on the element. The end's old
        element binding is deleted.

        Returns: the new element binding.
        """
        ...

    def clearEnd(self, side: int, /) -> None:
        """
        Make end 1 or 2 a free point where it is, deleting its element
        binding.
        """
        ...
