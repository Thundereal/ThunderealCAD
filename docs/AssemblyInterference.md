# Assembly: the interference check

Which components of an assembly overlap, and, when asked, which touch:
where they are, or over the frames of the last simulation. The static check
of the audit's phase 5 (`docs/AssemblyAudit.md` sec 8.2), and interference
over time (`docs/AssemblyDynamics.md` sec 7.3, P14). Built 2026-10-27.

## Use

**Assembly > Check Interference** (`Assembly_CheckInterference`, active with
an assembly in edit) opens a task panel:

- **Mode.** *Exact* works on the solids: the volume two components have in
  common, the exact distance between them; as slow as a boolean. *Fast*
  works on meshes of the solids, and finds that two overlap, not by how
  much.
- **Tolerance** (mm). Two components whose surfaces are within this of each
  other touch: a contact, not an interference, unless they overlap. In
  Fast it is also what covers the meshes' error, and is taken at least twice
  the mesh deflection; the summary says what was used.
- **Mesh deflection** (Fast only; Automatic is 1/10000 of each component's
  size, at least 0.01 mm).
- **Report touching components.** Off by default: mated parts touch by
  design.
- **Positions.** *Where they are*, or *Over the simulation's frames*: each
  frame of the last simulation run (Simulation, then Generate), the
  components where the frame puts them; the document is not moved. Enabled
  once a simulation has been run.
- **Frame step** (over frames): every this many frames; the last is always
  checked.

**Check** lists the pairs: the two components, Interference or Contact, and
the volume in common (Exact) or the distance (contacts). Over frames, a row
per pair and kind says in how many frames, from which to which, and the
largest volume (Exact); the frames are under it, each with its time.
Picking a row selects both components; for an Exact interference the solid
in common is drawn in red until another row is picked or the panel is
closed. Picking a frame shows the assembly at it, as the simulation's
playback does; picking a pair shows its worst frame (Exact), or its first.
The parts are put back where they were when the panel closes or checks
again. The choices are kept in the Assembly preferences (`InterferenceMode`,
`InterferenceTolerance`, `InterferenceDeflection`, `InterferenceContacts`,
`InterferenceFrameStep`).

From Python:

    results = assembly.findInterferences(
        mode="Exact", tolerance=0.0, contacts=False, objects=None, deflection=0.0,
        frames=None)

gives a list of dicts: `Object1`, `Object2`, `Kind` ("Interference" or
"Contact"), `Volume` (Exact, mm^3; 0 in Fast), `Distance` (contacts, mm;
Fast gives the tolerance), `Tolerance` (the one the pair was judged with),
`Overlap` (Exact: the solid in common, in the assembly's coordinates; else
None). `frames` is a list of frame numbers of the last simulation
(`range(assembly.numberOfFrames())` for all); then each result is one pair
at one frame and also has `Frame` and `Time` (s). A frame past the last is
a ValueError, as is `frames` with no simulation run.

## What is checked

The assembly's components (`getAssemblyComponents()`: a rigid sub-assembly
is one, a flexible one is its components), each with its shape taken
through its path from the assembly, so that a component of a flexible
sub-assembly is where it is in the assembly. Only solids: a component with
none is left out. Pairs whose bounding boxes are farther apart than the
tolerance are not looked at.

**Exact** (`Assembly::findInterferences`, `App/Interference.{h,cpp}`):
`BRepAlgoAPI_Common` of the two, non-destructive (the solids are the
document's own, and a destructive boolean may raise their tolerances), the
solids as arguments and tools. A volume above 1e-9 of the smaller
component's (faces in contact give a common of no volume, or of rounding)
is an interference, and the common is kept for the drawing. Otherwise, with
contacts asked for, `BRepExtrema_DistShapeShape`: within the tolerance, a
contact.

**Fast**:

1. Each distinct solid (TShape) is meshed once, on a copy
   (`BRepBuilderAPI_Copy`: the display's triangulation is left alone), and
   placed per instance -- a fastener used 200 times is meshed once.
2. Two solids overlap where their surfaces cross. An edge of a triangle of
   one crossing a triangle of the other, its ends on either side of the
   triangle's plane, one more than the tolerance from it and the other more
   than 1e-3 of it, is searched for over both components' bounding volume
   trees (`BRepExtrema_TriangleSet`'s BVH). Not both ends beyond the
   tolerance: a sphere sunk 0.08 mm into a face had no such edge, its
   mesh's rings near the pole 0.06 deep and within 0.001 of the face. The
   1e-3 leaves out the ends of edges on a face in contact, on its plane to
   rounding.
3. A crossing is only a candidate: where faces touch, an edge of one meets
   the other's boundary, and meshes that touch (a pin in its hole) cross
   within their error. It is confirmed by a point near it inside both solids
   by more than the tolerance, by exact point classification
   (`BRepClass3d_SolidClassifier`) -- any such point is a witness, so no
   point tried can make a false positive. Tried: along the bisector of the
   two triangles' normals, either way and either sign (the region inside
   both is a wedge between the surfaces; the step is sized to be 1.5 times
   the tolerance deep, as far as the edge is long); then a little along the
   edge, either way, and off its triangle, either way (the overlap of two
   boxes is bounded by faces of theirs, and its crossings are on them).
   After 64 unconfirmed candidates a pair is taken to touch.
4. One inside the other crosses nowhere, nor do two copies of a part in one
   place, whose surfaces coincide. With no crossing, one is wholly inside
   the other or wholly outside it, and any point inside it tells which: one
   is found 2 tolerances off the middle of a triangle of its mesh, either
   way, and classified -- not a vertex, which can be on the other's surface.
   Only when its mesh's box is in the other's.
5. Overlaps bounded by coplanar faces and tangent blends cross nowhere
   either: two filleted plates of one thickness slid into each other edge
   to edge. The middle of the part the two meshes' boxes have in common,
   and the middles of its halves along its longest side, are classified;
   for faces in contact that part is a slab, its middle on them.
6. With contacts asked for, a pair with no interference whose meshes come
   within the tolerance (`BRepExtrema_ShapeProximity`) is a contact.

**Over frames** the components are built and meshed once. At each frame a
component's move is its placement in the assembly at the frame (the
simulation's for each of it and its containers that the simulation moves,
the present one for the rest) times the inverse of its placement now; the
solids and meshes are moved, not rebuilt, and the triangles (and their
tree) of a moved component are made again only when a pair needs them. A
pair neither of whose components moved since the last frame checked is not
checked again: it has the last frame's finding. The work shows on the
progress bar and can be cancelled there.

## Measured (2026-10-27)

Over 102 frames, 20 sliders each driving a sphere (r 5) down into a box and
up through a disk, 60 components, defaults:

| | Fast | Exact |
|---|---|---|
| time | 2.7 s (0.85 s of it meshing) | 2.8 s |
| findings | 963 | 965 |

Over 52 frames, 10 sliders each moving a filleted plate with 16 holes (42
faces) over and across another:

| | Fast | Exact |
|---|---|---|
| time | 1.3 s | 148 s |
| findings | 514 | 514 |

On analytic solids the booleans are cheap and Exact is as fast; Fast pays
from a few dozen faces on. One frame of the plates where they are: Fast
0.1 s, Exact 2.3 s. Timed with a WSL build sharing the box; earlier runs
under heavier load were up to 1.5 times slower.

What Fast misses here, Exact found: two sphere caps of under 0.1 mm^3, some
0.08 mm deep, at a tolerance of 0.02 mm -- the mesh's rings, 0.6 mm apart at
the default deflection, straddle such a cap without an edge across it. At a
deflection of 0.005 mm they are found. Before the confirmation on the
bisector, Fast missed 231 interferences of the first case, up to 126 mm^3;
before the classification of an inner point and of the middle of the boxes'
common part, 10 of the second (two copies in one place) and 2 more (the
filleted plates edge to edge). Each of these is a test now
(`AssemblyTests/TestInterference.py`). The non-destructive boolean fixed
Exact, which failed (`IsDone()` false) where a sphere touched a disk's face
at a point.

## Tests

`AssemblyTests/TestInterference.py`:

- `TestInterference`, on an assembly whose answers are known: an overlap of
  200 mm^3, a box inside another, two bars crossing (8 mm^3 in common, no
  vertex of either inside the other), faces in contact, a pin snug in its
  hole, a gap of 0.05 mm, a Link, two copies of a box in one place, a box
  in another on five of its faces, two filleted plates edge to edge. Both
  modes find the same pairs; Exact gives the volumes to 1e-6.
- `TestInterferenceOverFrames`: a slider drives a box up through a plate,
  and two boxes that never move overlap. Every frame's volume against the
  playback's own common, the document left where it was, the unmoving pair
  at every frame, the frames' times in order, only the frames asked for,
  bad frames refused; Fast finds the same frames, but for overlaps under
  1 mm^3.

## Limits

- Fast finds that two components overlap, not by how much, and an overlap
  no deeper than a few times the tolerance can be taken for a contact; Exact
  is the authority. Fast's tolerance is at least twice the deflection, so
  checking a press fit of 0.01 mm takes Exact, or a deflection under
  0.005 mm.
- Over frames, an interference that begins and ends between two frames
  checked is not seen: the frames are the simulation's output steps (and
  the frame step's).

## Not done

- Interference while dragging (stop at collision, highlight the faces): the
  Fast path, its meshes and moved triangles, is the start.
- Exclusions (hidden components, a fasteners group) and clearance thresholds
  per pair.
- Faces in contact highlighted, the touching area measured.
- The work in a worker or another process (`docs/ComputeBoundaries.md`), and
  the pairs of a frame in parallel.
