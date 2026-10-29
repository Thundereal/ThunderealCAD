# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from typing import Any, Final, Optional

from Base.Metadata import constmethod, export

from App.Part import Part
from App.DocumentObject import DocumentObject

@export(Include="Mod/Assembly/App/AssemblyObject.h", Namespace="Assembly")
class AssemblyObject(Part):
    """
    This class handles document objects in Assembly

    Author: Ondsel (development@ondsel.com)
    License: LGPL-2.1-or-later
    """

    @constmethod
    def solve(self, enableUndo: bool = False, /) -> int:
        """
        Solve the assembly and update part placements.

        Args:
        enableRedo: Whether the solve save the initial position of parts
        to enable undoing it even without a transaction.
        Defaults to `False` ie the solve cannot be undone if called
        outside of a transaction.

        Returns:
        0 in case of success, otherwise the following codes in this order of
        priority:
        -6 if no parts are fixed.
        -4 if over-constrained,
        -3 if conflicting constraints,
        -5 if malformed constraints
        -1 if solver error,
        -2 if redundant constraints.
        """
        ...

    @constmethod
    def generateSimulation(self, simulationObject: DocumentObject, /) -> int:
        """
        Generate the simulation.

        Args:
        simulationObject: The simulation Object.

        Returns:
        0 in case of success, otherwise the following codes in this order of
        priority:
        -6 if no parts are fixed.
        -4 if over-constrained,
        -3 if conflicting constraints,
        -5 if malformed constraints
        -1 if solver error,
        -2 if redundant constraints.
        """
        ...

    @constmethod
    def updateForFrame(self, index: int, /) -> None:
        """
        Update entire assembly to frame number specified.

        Args:
            index: index of frame.

        Returns: None
        """
        ...

    @constmethod
    def numberOfFrames(self) -> int:
        """Return Number of frames"""
        ...

    @constmethod
    def getSimulationResults(self) -> dict:
        """
        The values the last simulation reported with its frames
        (docs/AssemblyDynamics.md sec 8.4); a Dynamic one reports them.

        Returns: a dict: Times (s, one per frame) and Channels, a list of
        dicts, each with Object (the joint or the spring; None when gone),
        Name (its name), Quantity and Values (one per frame). Quantity is
        "Force" or "Torque", a joint's reaction on the part of its first
        reference, vectors in the assembly's axes (mN, and mN mm about the
        joint's coordinate system on that part); "MotorTorque" (mN mm) or
        "MotorForce" (mN), what a motion's motor applies along its joint's
        axis; "SpringForce" (mN, positive pushing the ends apart) or
        "SpringLength" (mm), a spring-damper's; "SpringTorque" (mN mm,
        positive turning the joint up) or "SpringAngle" (rad, the joint's
        turn as its limits read it), a torsion spring's. Unique is False
        for a reaction a redundant joint shares with other joints: the run
        took out the equations that repeat others, and the values are one
        split of the load among them, not the only one. Simulation: the
        study the frames are of (None before a run, or once it is deleted).
        """
        ...

    @constmethod
    def getSimulationPlacements(self, obj: Any) -> list:
        """
        The Placement the last simulation gives obj at each of its frames, as
        playback writes it; the document is left as it is.

        Args:
            obj: a component the simulation moves.

        Returns: a list of Placements, one per frame (numberOfFrames());
        empty when the simulation does not move obj, or there is none.
        """
        ...

    @constmethod
    def updateSolveStatus(self) -> Any:
        """updateSolveStatus()

        Args: None

        Returns: None"""
        ...

    @constmethod
    def undoSolve(self) -> Any:
        """Undo the last solve of the assembly and return part placements to their initial position.

        undoSolve()

        Returns: None"""
        ...

    @constmethod
    def ensureIdentityPlacements(self) -> None:
        """
        Makes sure that LinkGroups or sub-assemblies have identity placements.
        """
        ...

    @constmethod
    def refreshFrozenShape(self) -> None:
        """
        Take the compound of the visible components again, into the Parts
        group, which is made if there is none. Freezing takes it once.
        """
        ...

    @constmethod
    def clearUndo(self) -> None:
        """
        Clear the registered undo positions.
        """
        ...

    def getGroup(self, role: str, create: bool = False, /) -> Optional[DocumentObject]:
        """
        The assembly's group of a role, "Joints", "Simulations" or "Views":
        its aux group (Assembly::AssemblyGroup), else upstream's group of the
        role in a file not converted, else with create a new aux group.

        Returns: the group, or None.
        """
        ...

    @constmethod
    def isPartConnected(self, obj: DocumentObject, /) -> bool:
        """
        Check if a part is connected to the ground through joints.
        Returns: True if part is connected to ground.
        """
        ...

    @constmethod
    def isJointConnectingPartToGround(self, joint: DocumentObject, prop_name: str, /) -> Any:
        """
        Check if a joint is connecting a part to the ground.

        Args:
        - joint: document object of the joint to check.
        - prop_name: string 'Part1' or 'Part2' of the joint.

        Returns: True if part is connected to ground.
        """
        ...

    @constmethod
    def isPartGrounded(self, obj: DocumentObject, /) -> Any:
        """
        Check if a part has a grounded joint.

        Args:
        - obj: document object of the part to check.

        Returns: True if part has grounded joint.
        """
        ...

    @constmethod
    def exportAsASMT(self, file_name: str, /) -> None:
        """
        Export the assembly in a text format called ASMT.

        Args:
        - fileName: The name of the file where the ASMT will be exported.
        """
        ...

    @constmethod
    def getDownstreamParts(
        self, start_part: DocumentObject, joint_to_ignore: DocumentObject, /
    ) -> list[DocumentObject]:
        """
        Finds all parts connected to a start_part that are not connected to ground
        when a specific joint is ignored.

        This is used to find the entire rigid group of unconstrained components that
        should be moved together during a pre-solve operation or a drag.

        Args:
            start_part: The App.DocumentObject to begin the search from.
            joint_to_ignore: The App.DocumentObject (a joint) to temporarily
                             suppress during the connectivity check.

        Returns:
            A list of App.DocumentObject instances representing the downstream parts.
        """
        ...

    @constmethod
    def preDrag(self, parts: list[DocumentObject], mass: float = 10.0, /) -> None:
        """
        Start dragging parts, as the 3D view does when the mouse grabs one.

        Solves the assembly with the parts joined by Fixed joints bundled
        into one body, and keeps that solver system for the drag steps.

        Args:
            parts: the parts that will be dragged.
            mass: the mass the dragged parts get in the drag steps (every
                other part weighs 1), so that they lead and the rest give
                way. The 3D view uses 10.
        """
        ...

    @constmethod
    def doDragStep(self) -> bool:
        """
        One drag step: solve for the Placement written on the dragged parts.

        Set the dragged parts' Placement to where the mouse took them, then
        call this; the other parts move to follow.

        Returns: True when the step was solved and the placements written;
        False when no drag is in progress or the step failed, in which case
        the parts stay where the last good step put them and the drag can
        go on (a warning says so once per drag).
        """
        ...

    @constmethod
    def postDrag(self) -> None:
        """
        End a drag: a last full solve from where the drag left the parts.
        """
        ...

    @constmethod
    def getDragStatus(self) -> dict:
        """
        The current or last drag, as preDrag() began it.

        Returns: a dict with Steps (drag steps attempted), Failures (steps
        that failed) and LastStepFailed (bool).
        """
        ...

    @constmethod
    def getMassProperties(self) -> dict:
        """
        The mass properties of the components (docs/AssemblyDynamics.md sec
        5.2), from their shapes and their materials' density, or the default
        material's (the preference Mod/Assembly/DefaultMaterial, Aluminum
        Generic) where a material gives none.

        Returns: a dict: Components, a list of dicts, one per component
        (getAssemblyComponents()); Total, a dict of them all together. Each
        dict has Object (None for the total), Origin ("Shape", "Assumed":
        the default material's density for some of it, "Override", or
        "None": no solid), Material (its name; empty for several), Density
        (kg/mm^3; 0 for several), Volume (mm^3), Mass (kg), CenterOfMass
        (a vector, mm, in the assembly's coordinates) and Inertia (xx, yy,
        zz, xy, xz, yz: the inertia tensor about the centre of mass, along
        the assembly's axes, kg mm^2).
        """
        ...

    def getBodyInertias(self) -> list:
        """
        The solver's bodies, as a solve builds them, and each one's mass
        properties for dynamics (docs/AssemblyDynamics.md sec 6.1): its
        components' (getMassProperties()) summed, a rigid group's members
        into one body.

        Returns: a list of dicts, one per body with mass: Objects (the
        components it is), Placement (the body's frame, in the assembly),
        Mass (kg), CenterOfMass (mm, in the body's frame), Inertia (xx, yy,
        zz, xy, xz, yz, kg mm^2, about the centre of mass along the body's
        axes), Origin ("Shape", "Override" or "AssumedDensity"). The
        assembly is left as a solve leaves it before solving.
        """
        ...

    @constmethod
    def getCollisionMarkers(self) -> list:
        """
        Collision while dragging (docs/AssemblyInterference.md): the markers of
        where drags brought components into collision, one for each pair a
        drag brought into collision, kept until cleared and not saved.

        Returns: a list of dicts: First, Second (the objects, None when gone),
        Centre (where they met, in the assembly's coordinates), Direction (a
        unit vector out of the component hit, towards the other), Kind
        ("Interference", "Contact" or "Clearance", as findInterferences()
        has them) and Contact (Kind is "Contact"), Volume, Area, Distance,
        Clearance, Faces1 and Faces2 (as findInterferences() has them),
        FromDrag (a drag's, not the check's).
        """
        ...

    def clearCollisionMarkers(self, checksOnly: bool = False, /) -> None:
        """Removes the markers of getCollisionMarkers(); with checksOnly, those
        the interference check added (addCollisionMarker()) only."""
        ...

    def addCollisionMarker(
        self,
        result: dict,
        /,
    ) -> None:
        """
        A marker of a result of findInterferences() (a dict with Object1,
        Object2, Centre, Direction and Kind; Volume, Area, Distance,
        Clearance, Faces1, Faces2 if it has them), unless the pair has one
        of that kind there already.
        """
        ...

    @constmethod
    def getDragCollisions(self) -> list:
        """
        Collision while dragging (docs/AssemblyInterference.md), with the
        DragCollision preference at 1 (highlight) or 2 (stop) when the drag
        began: the pairs of components the last drag step brought into
        collision, or, stopped, would have.

        Returns: a list of tuples of two objects; empty with no drag, or the
        preference off.
        """
        ...

    @constmethod
    def getSolveStatus(self) -> dict:
        """
        The diagnosis of the last solve, as the messages panel shows it.

        Returns: a dict with
            DoF: int, the degrees of freedom left (6 per solver body, minus
                the equations the solver kept);
            SolverStatus: int, 0 when the last solve converged, -1 when it failed;
            Conflicting: list of joint names whose equations contradict the others;
            Redundant: list of joint names with at least one equation the solver
                dropped as redundant (conflicting ones included);
            PartiallyRedundant: the subset of Redundant with only some of their
                equations dropped;
            Malformed: list of active joints whose references are set but
                unusable (both on one part, or a sub-element that does not resolve).
        """
        ...

    @constmethod
    def getSolverResiduals(self) -> dict:
        """
        How far each joint is from satisfied at the poses the solver holds now
        (after a solve, or a drag step), by the solver-neutral checker: the
        same equations for every backend, each joint's largest error in mm
        (positions, distances) or rad (angles; the cosine for an orientation
        equation). A coupler (gears, rack and pinion, screw) is measured
        against the relation it had when the solve started.

        Returns: a dict {joint name: error}; empty when nothing was solved.
        """
        ...

    @constmethod
    def findInterferences(
        self,
        mode: str = "Exact",
        tolerance: float = 0.0,
        contacts: bool = False,
        objects: list = None,
        deflection: float = 0.0,
        frames: list = None,
        rules: bool = True,
    ) -> list:
        """
        Which components overlap, and, with contacts, which touch
        (docs/AssemblyInterference.md).

        Args:
            mode: "Exact" -- on the B-rep: the volume in common, the exact
                distance; or "Fast" -- on meshes of the solids: their
                surfaces crossing by more than the tolerance, confirmed by a
                point inside both; it finds that they overlap, not by how
                much.
            tolerance: mm. Components within this of each other touch, and
                interfere only when they overlap (Exact: by a volume; Fast:
                by a crossing deeper than this). Fast takes it at least twice
                the meshes' deflection.
            contacts: report the pairs that touch too.
            objects: only these components (default: all of them).
            deflection: mm, Fast only: the meshes' chord error; 0 is 1e-4 of
                each shape's bounding box diagonal, at least 0.01 mm.
            frames: frame numbers of the last simulation (numberOfFrames()):
                each component where the frame puts it, the document left
                where it is (default: the components where they are).
            rules: apply the assembly's Interference properties: the
                components left out (InterferenceExcluded,
                InterferenceIgnoreHidden), the pairs ignored
                (InterferenceIgnoredPairs), the clearances pairs keep
                (InterferenceClearances).

        Returns: a list of dicts, one per pair (and frame): Object1, Object2, Kind
        ("Interference", "Contact", or "Clearance": closer than the clearance
        the rules ask of the pair, which Clearance gives, in mm, 0 for none,
        and not overlapping), Volume (Exact, mm^3; 0 in Fast), Area (a contact's
        or a clearance's: the area the two lie face to face over, mm^2, from
        their meshes), Faces1 and Faces2 (the faces of each where they meet,
        by their names from the assembly, "A.B.Face3": crossing, or in the
        solid in common, for an interference; face to face for a contact),
        Distance (contacts, mm; Fast gives the tolerance), Tolerance (the one
        the pair was judged with) and Overlap (Exact: the solid in common, in
        the assembly's coordinates; else None); with frames, also Frame and
        Time (s). Each has Centre, where the two meet, and Direction, a unit
        vector out of Object2 towards Object1 (an interference, Fast: at the
        middle of the curve along which their meshes cross; Exact: at the
        centre of mass of the solid in common. A contact: the middle of the
        vertices of either mesh near the other, Exact meshing them for it);
        None when not found.
        """
        ...

    @constmethod
    def getSolverMetric(self) -> dict:
        """
        How the last solve weighed movement (docs/AssemblySolverInterface.md
        sec 6.20): each solved object's point whose displacement counts, in the
        object's own frame (the centre of its body's joints), the inertia a turn
        of the object counts with, and its solver body (objects bundled into one
        body share it), and the body's mass (a group of bodies weighs as many as
        it has) -- the distance of a solve from its start is the sum over the
        bodies of mass * |displacement|^2 + inertia * |turn|^2 / 4.

        Returns: a dict {object name: (Vector, inertia, body, mass)}; empty when
        nothing was solved.
        """
        ...
    def makeJoint(self, jointType: str, pick1: tuple, pick2: tuple, /) -> DocumentObject:
        """
        Make a C++ joint (Assembly::Joint) in the assembly's joint group, its
        two sides element bindings made from picks (docs/AssemblyObjectModel.md
        sec 7.1).

        Args:
        jointType: a name of JointObject.JointTypes ("Fixed", "Revolute", ...).
        pick1, pick2: (component, subname) or (component, subname, vertex):
            a direct child of the assembly, the element in it as a selection
            gives it ("Face6", "Pad.Face6"), and optionally a vertex placing
            the joint's origin on the element, as upstream picks it.

        The element is declared in the component when it declares elements
        (an App::Part, a Body, an assembly, or a link to one), reusing one
        that names the same geometry; otherwise it is a convenience element
        in the assembly's own Elements group.

        Returns: the joint.
        """
        ...
    Joints: Final[list]
    """A list of all joints this assembly has."""
