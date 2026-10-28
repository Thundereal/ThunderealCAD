# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from typing import Optional, Tuple

from Base.Metadata import constmethod, export
from Base.Placement import Placement

from App.DocumentObject import DocumentObject

@export(
    Include="Mod/Assembly/App/Physics.h",
    Namespace="Assembly",
)
class FramedItem(DocumentObject):
    """
    A Physics item at a frame on a component (an applied load, a bushing): a
    free frame, or an element's frame through an element binding the item
    owns (docs/AssemblyDynamics.md).

    Author: Zheng Lei (realthunder.dev@gmail.com)
    License: LGPL-2.1-or-later
    """

    @constmethod
    def getEnd(self) -> Tuple[Optional[DocumentObject], Placement]:
        """
        The component the frame is on (None: the ground) and the frame in its
        coordinates.
        """
        ...

    @constmethod
    def getEndPlacement(self) -> Placement:
        """
        The frame in the assembly's coordinate system now.
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
        Put the frame on an element, picked as a joint's side is: a direct
        child of the assembly, the element in it as a selection gives it, and
        optionally a vertex placing the origin on the element. The old element
        binding is deleted.

        Returns: the new element binding.
        """
        ...

    def clearEnd(self) -> None:
        """
        Make the frame a free one where it is, deleting its element binding.
        """
        ...
