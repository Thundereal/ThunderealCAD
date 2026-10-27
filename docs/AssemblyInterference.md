# Assembly: the interference check

Which components of an assembly overlap, and, when asked, which touch. The
static check of the audit's phase 5 (`docs/AssemblyAudit.md` sec 8.2), and
the one-frame case of interference over time (`docs/AssemblyDynamics.md`
sec 7.3). Built 2026-10-27.

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
  size).
- **Report touching components.** Off by default: mated parts touch by
  design.

**Check** lists the pairs: the two components, Interference or Contact, and
the volume in common (Exact) or the distance (contacts). Picking a row
selects both components; for an Exact interference the solid in common is
drawn in red until another row is picked or the panel is closed. The choices
are kept in the Assembly preferences (`InterferenceMode`,
`InterferenceTolerance`, `InterferenceDeflection`, `InterferenceContacts`).

From Python:

    results = assembly.findInterferences(
        mode="Exact", tolerance=0.0, contacts=False, objects=None, deflection=0.0)

gives a list of dicts: `Object1`, `Object2`, `Kind` ("Interference" or
"Contact"), `Volume` (Exact, mm^3; 0 in Fast), `Distance` (contacts, mm;
Fast gives the tolerance), `Tolerance` (the one the pair was judged with),
`Overlap` (Exact: the solid in common, in the assembly's coordinates; else
None).

## What is checked

The assembly's components (`getAssemblyComponents()`: a rigid sub-assembly
is one, a flexible one is its components), each with its shape taken
through its path from the assembly, so that a component of a flexible
sub-assembly is where it is in the assembly. Only solids: a component with
none is left out. Pairs whose bounding boxes (from the geometry, not a mesh)
are farther apart than the tolerance are not looked at.

**Exact** (`Assembly::findInterferences`, `App/Interference.{h,cpp}`):
`BRepAlgoAPI_Common` of the two; a volume above 1e-9 of the smaller
component's (faces in contact give a common of no volume, or of rounding) is
an interference, and the common is kept for the drawing. Otherwise, with
contacts asked for, `BRepExtrema_DistShapeShape`: within the tolerance, a
contact.

**Fast**:

1. Each distinct solid (TShape) is meshed once, on a copy
   (`BRepBuilderAPI_Copy`: the display's triangulation is left alone), and
   placed per instance -- a fastener used 200 times is meshed once.
2. Two solids overlap exactly when their surfaces cross. An edge of a
   triangle of one crossing a triangle of the other, from more than the
   tolerance on one side of its plane to more than it on the other, is
   searched for over both components' bounding volume trees
   (`BRepExtrema_TriangleSet`'s BVH).
3. A crossing is only a candidate: where faces touch, an edge of one meets
   the other's boundary, and meshes that touch (a pin in its hole) cross
   within their error. It is confirmed by a point near it inside both solids
   by more than the tolerance -- a little along the edge, either way, then a
   little off the edge's own triangle, either way -- by exact point
   classification (`BRepClass3d_SolidClassifier`). Stepping along the edge
   alone is not enough: the overlap of two boxes is bounded by faces of
   theirs, and such a point stays on a surface. After 64 unconfirmed
   candidates a pair is taken to touch.
4. One inside the other crosses nowhere: a point of the inner one is
   classified when its box is inside the other's.
5. With contacts asked for, a pair with no confirmed crossing whose meshes
   come within the tolerance (`BRepExtrema_ShapeProximity`) is a contact.

An overlap thinner than about the tolerance can be taken for a contact in
Fast; Exact is the authority.

## Tests

`AssemblyTests/TestInterference.py`, on an assembly whose answers are known:
an overlap of 200 mm^3, a box inside another, two bars crossing (8 mm^3 in
common, no vertex of either inside the other), faces in contact, a pin snug
in its hole, a gap of 0.05 mm, a Link. Both modes find the same pairs; Exact
gives the volumes to 1e-6.

## Not done

- Interference while dragging (stop at collision, highlight the faces), and
  over a simulation's frames (`docs/AssemblyDynamics.md` sec 7.3): the Fast
  path is the start of both.
- Exclusions (hidden components, a fasteners group) and clearance thresholds
  per pair.
- Faces in contact highlighted, the touching area measured.
- The work in a worker or another process (`docs/ComputeBoundaries.md`).
