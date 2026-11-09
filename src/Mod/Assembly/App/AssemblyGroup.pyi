# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from Base.Metadata import export

from App.DocumentObject import DocumentObject

@export(
    Include="Mod/Assembly/App/AssemblyGroup.h",
    Namespace="Assembly",
)
class AssemblyGroup(DocumentObject):
    """
    The assembly's Joints, Simulations or Views group: a view of the
    assembly's own children by role (docs/AssemblyObjectModel.md sec 3), in
    place of upstream's JointGroup, SimulationGroup and ViewGroup.

    Author: Zheng Lei (realthunder.dev@gmail.com)
    License: LGPL-2.1-or-later
    """

    def newObject(self, type: str, name: str = "", /) -> DocumentObject:
        """
        Make an object of the type in the assembly, where the group shows it
        when it is of the group's role.

        Returns: the new object.
        """
        ...

    def addObject(self, obj: DocumentObject, /) -> None:
        """
        Give the assembly the object, for the group to show when it is of the
        group's role.
        """
        ...
