# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from typing import Optional

from Base.Metadata import constmethod, export
from Base.Placement import Placement

from App.DocumentObject import DocumentObject

@export(
    Include="Mod/Assembly/App/Joint.h",
    Namespace="Assembly",
)
class Joint(DocumentObject):
    """
    A joint whose two sides are element bindings, children of its assembly
    (docs/AssemblyObjectModel.md sec 7.1).

    Author: Zheng Lei (realthunder.dev@gmail.com)
    License: LGPL-2.1-or-later
    """

    @constmethod
    def getComponent(self, side: int, /) -> Optional[DocumentObject]:
        """
        The component side 1 or 2 moves, a direct child of the assembly, or
        None while the side is not set.
        """
        ...

    @constmethod
    def getFrame(self, side: int, /) -> Placement:
        """
        The joint coordinate system of side 1 or 2 in the assembly's
        coordinate system, resolved now: the element through its component,
        and the offsets applied.
        """
        ...

    def setSide(
        self,
        side: int,
        component: DocumentObject,
        subname: str,
        vertex: str = "",
        local: bool = False,
        /,
    ) -> DocumentObject:
        """
        Make side 1 or 2 from a pick: a direct child of the assembly, the
        element in it as a selection gives it, and optionally a vertex placing
        the origin on the element (AssemblyObject.makeJoint). With local, the
        element is a convenience element of the assembly even when the
        component declares elements: a migration does not edit the
        components. The side's old element binding is deleted unless another
        joint uses it.

        Returns: the new element binding.
        """
        ...

    def redrawMarkers(self) -> None:
        """
        Have the joint's view draw its markers where its frames are now: a
        solve moves the components without a recompute.
        """
        ...

    def clearSide(self, side: int, /) -> None:
        """
        Unset side 1 or 2, deleting its element binding unless another joint
        uses it.
        """
        ...
