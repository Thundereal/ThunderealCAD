# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from typing import Any

from Base.Metadata import constmethod, export

from App.DocumentObject import DocumentObject

@export(Include="Mod/Fluid/App/Study.h", Namespace="Fluid")
class Study(DocumentObject):
    """
    A fluid study: its walls, the fluid at the start, its monitors, the run
    and the results it keeps.

    License: LGPL-2.1-or-later
    """

    @constmethod
    def buildModel(self) -> dict:
        """
        The study as a solver is given it, summed up: Walls and Fluids (how
        many), Triangles, Monitors, Resolution (m), SpaceMin and SpaceMax (m),
        Gravity (m/s^2), TimeEnd and OutputInterval (s), Signature (str), and
        Bodies: the parts the fluid moves, counted among the Walls, each
        Source, Mass (kg), Centre (m) and Inertia (about the centre, kg m^2:
        xx, yy, zz, xy, xz, yz), and Driven: a mechanism's part, moved by
        the study's Mechanism under the fluid's push.
        Inlets: the faces the fluid comes in through, each Source, Direction
        (the way the fluid goes), Speed (m/s), Area (m^2), Reynolds (the
        fluid's density, the speed and the width of a round face as large
        as the inlet -- of a steady flow, as its part of the fluid's
        space -- over the viscosity), Middle (m), Start and End (s; an End not
        after the Start is the run's).
        Open: which sides of the space are open, -x, +x, -y, +y, -z, +z.
        Analysis: FreeSurface or Steady. A steady flow has as well
        Iterations; Turbulence, 'Laminar' or 'KOmegaSST'; Outlets, each
        Source, Pressure (Pa) and Area (m^2); and
        Domain, the space the fluid fills: Triangles, Volume (m^3) and
        Areas (m^2) -- of its walls, then of each inlet, then of each
        outlet. Its space is the fluid's box.
        """
        ...

    def run(self) -> int:
        """
        Runs the study on its backend and keeps the results: start() and
        finish(). 0 when it ran, else a negative code: -1 failed, -2 the
        backend cannot run this study, -3 the backend or what it needs was
        not found, -4 nothing to simulate, -7 cancelled. The Status property
        says why.
        """
        ...

    def start(self) -> int:
        """
        Starts a run in a thread of its own and returns at once; codes as
        run()'s. Follow it with getProgress() and end it with finish().
        """
        ...

    @constmethod
    def getProgress(self) -> dict:
        """
        Running (a run was started and not finished), Done (finish() will
        not wait), Time (of the last frame, s) and Frames (made so far).
        """
        ...

    def cancel(self) -> None:
        """
        Asks the run to stop; finish() then returns -7.
        """
        ...

    def finish(self) -> int:
        """
        Waits for the run started and keeps its results; codes as run()'s.
        """
        ...

    @constmethod
    def getResults(self) -> Any:
        """
        The kept results, or None: Backend, Version, Kind ('Particles',
        'Surface', 'Fields'), Times (the frames', s), Frames (how many), MonitorTimes
        (the monitors' own times, s: a solver reports them far oftener than
        it makes frames) and Monitors, each Object, Kind, Unit and Values
        (one per monitor time); and Bodies, the parts the fluid moved, each
        Object and Source. From a solver that keeps account of its energy,
        Energy too: Times (s) and, at each, in J, the fluid's Kinetic and
        Potential energy, its Work so far on the parts it moves, and of a
        mechanism's parts what their lag behind the fluid Created and what
        was Exchanged; Fed, whether inlets fed the run; and what is made of
        it past the run's first quarter -- Judged, whether there was
        enough to; BudgetJudged, Rise, RiseLimit and Rose, the most the
        three together rose as a share of the fluid's greatest kinetic
        energy (they cannot; not judged of a run that was fed);
        HandOverJudged, Made, MadeLimit and Lagged, the share of what was
        exchanged that the lag created. Rose or Lagged: the results are
        not to be trusted. Neither: nothing was found, which is not to say
        the particles are fine enough. Steady, whether the results are a
        steady flow's: its Times and MonitorTimes are then the steps it had
        settled for; Settled, whether it settled before the steps it was
        given were up; and it has Openings, each inlet then each outlet:
        Object, Source, Kind ('Inlet', 'Outlet') and, one per frame,
        Pressure (the mean over the face, Pa) and Flow (m^3/s, in through
        an inlet and out through an outlet); Inside, the frames that
        have the fluid inside kept (getInside(), getSection()); Turbulence,
        'Laminar' or 'KOmegaSST'; Reynolds, the greatest Reynolds number of
        its inlets (0 when not kept); and WallUnits, of a turbulent flow
        how far the first cell at a wall is from it in the wall's own units
        (y+), the least, the most and the mean, or None.
        """
        ...

    @constmethod
    def getFrame(self, index: int, /) -> dict:
        """
        One kept frame, read from the document when asked for: Time (s),
        Count (of points), Points (bytes: float32 x, y, z each, mm) and, for
        a surface, Triangles (bytes: int32, three per triangle); for
        fields the triangles are the faces the fluid touches, with Values
        (bytes: float32, the pressure at each point, Pa); and Bodies,
        where the parts the fluid moved are, in the results' order: each a
        Placement, the motion from where the part stood at the start.
        """
        ...

    @constmethod
    def getInside(self, index: int, /) -> dict:
        """
        The fluid inside at a kept frame, of the frames that have it (the
        results' Inside lists them; of a steady flow its last): Count (of
        the solver's cells), Points (bytes: float32 x, y, z each, mm, each
        cell's centre), Velocity (bytes: float32 x, y, z each, m/s) and
        Pressure (bytes: float32, Pa). Count is 0 for a frame without.
        """
        ...

    @constmethod
    def getSection(self, index: int, base: Any, normal: Any, step: float = 0.0, /) -> dict:
        """
        A flat cut through the fluid inside at a kept frame, by the plane
        through base (mm) across normal: Count (of points), Points (bytes:
        float32 x, y, z each, mm: a grid on the plane, those inside the
        fluid), Triangles (bytes: int32, three per triangle), Velocity
        (bytes: float32 x, y, z each, m/s), Pressure (bytes: float32, Pa),
        Outline (bytes: float32, six a piece: the two ends, mm, of where the
        plane cuts a face the fluid touches), Grid (bytes: int32, two a
        point: how many steps along the plane's two axes it is) and Step
        (mm, between the grid's points: as given, or half the run's
        resolution). Count is 0
        where the plane misses the fluid or the frame has none kept.
        """
        ...

    @constmethod
    def getPaths(self, index: int, seeds: bytes, interval: float = 0.0, most: int = 2000, /) -> dict:
        """
        Tracers let go in the fluid inside at a kept frame and followed
        through it, one from each seed (bytes: float32 x, y, z each, mm): a
        point each interval (s), most points a path at most. With no
        interval given, one that takes the fastest of the fluid half a cell
        a point. Count (of paths), Points (bytes: float32 x, y, z each, mm,
        one path after another), Speeds (bytes: float32, m/s, the fluid's at
        each point), Starts (bytes: int32, where each path starts in the
        points and, after the last, where they end) and Interval (s). A
        path ends where the tracer leaves the fluid or stands still; no
        paths of a frame without the fluid inside.
        """
        ...

    @constmethod
    def getBodies(self, index: int, /) -> list:
        """
        Where the parts the fluid moved are at a kept frame, in the order
        of getResults()'s Bodies: each a Placement, the motion from where
        the part stood at the start. Read without the frame's points.
        """
        ...

    @constmethod
    def getSurface(self, index: int, /) -> dict:
        """
        One kept frame as a closed surface to draw: Time (s), Bodies (as
        getFrame()'s), Count (of
        points), Points (bytes: float32 x, y, z each, mm), Normals (bytes:
        float32, one a point, outwards) and Triangles (bytes: int32, three
        per triangle, wound outwards). A frame of particles is wrapped in
        the surface around them, each a cube of the run's resolution,
        particles apart from the body as drops that run together where
        they are near; those with next to none about them (one alone, a
        pair, three in a row) are no part of it but its Spray (bytes:
        float32 x, y, z each, mm). A frame that is a surface has no
        Normals and no Spray and is given as it is kept.
        """
        ...

    @constmethod
    def getWallPoints(self, index: int, /) -> dict:
        """
        The walls as getSurface() is told of them at one kept frame: Points
        (bytes: float32 x, y, z each, mm), points on the walls half the
        run's resolution apart, the parts the fluid moved where the frame
        has them; and Normals (bytes: float32, one a point, out of the
        solid). Beside a wall each particle's mirror image across it is in
        the field the surface is cut from.
        """
        ...

    @constmethod
    def isStale(self) -> bool:
        """
        Whether the kept results are of another model than the study is now.
        """
        ...
