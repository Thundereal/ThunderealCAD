# Topological Naming: what is left to build

Status: design and evaluation, written 2026-09-05 against this fork at
`d6435599a2`.  Nothing here is built yet.

The topological naming problem itself is solved in this fork -- element
maps, mapped names, the string hasher and the history-walking recovery
have shipped for years, and `CLAUDE.md` correctly says it is no longer
the active focus.  This document is about two things that were never
finished on top of it:

1. **A pre-task**: every element reference in a document is stored as a
   name and nothing else.  The geometry it was made against exists only
   in memory, for the duration of one session.  Now that shape storage
   is content-addressed and deduplicated, that geometry can be *named*
   in the file for almost nothing, and a reference that breaks can then
   be shown to the user instead of silently repointed or silently lost.
   Section 2.
2. **The main task**: TechDraw does not use element maps at all.  Not
   partially -- at all.  Section 3.

Related reading: `docs/SharedShapeStorage.md` (the content-addressed
blob store the pre-task rides on, especially sec 12.1 to 12.3),
`docs/FileBlobsManager.md` sec 13 (the blob index format),
`docs/TechDrawPortAndSection.md` (the upstream port, now complete, and
the section-view work the main task builds on).

Probe scripts for everything measured here were kept in the session
scratchpad, following the `DrawBrokenView` convention of not committing
them.  **Section 6.3 restates the three models so every number here can
be reproduced without them.**

**Resuming?  The open task is section 9** (unstable indices in element
names, handoff 2026-11-12).  For TechDraw, start at section 8: T0 is
built and T1 is next.  Section 7 is the pre-task, complete for the
`Shape` property -- it re-plans that work around versioned base shapes
held as dynamic properties on the referenced feature, closes section
6's open decisions, and 7.11 to 7.18 record what was built.  Section 6
is kept for its state of play and the measurement recipes.


## 1. The short answers

1. **Is there already a base-shape reference internally?**  Yes.
   `Part::Feature::_elementCache` (`src/Mod/Part/App/PartFeature.cpp:1206`)
   snapshots the referenced geometry immediately before a shape changes,
   and `searchElementCache` uses it to re-find the element afterwards.
   It is real, it works, and it is **never written to the file**.  It
   also holds only the sub-shape, not the base shape, so the recovery it
   can do is exact geometric equality and nothing better.

2. **Would storing it be expensive?**  No, and this is the measurement
   that makes the pre-task worth doing now rather than two years ago.
   A shape is already one content-addressed blob named after the
   property that owns it, listed in `blobs/Content.xml` with the list of
   every referrer that shares it.  A reference that records the base
   shape it was resolved against records a **40-character hash**, and in
   the healthy case that hash is already in the file.  Section 2.5.

3. **Is the existing recovery actually broken?**  Less than expected,
   and the honest framing matters.  Measured, it repairs more than it
   fails at -- but it does so **silently, automatically, with no record
   and no way for the user to check it**, and when it does fail it fails
   to `?Face3` with the original geometry gone forever.  The pre-task is
   about making repair *reviewable*, not about making it possible.
   Section 2.3.

4. **Can TechDraw carry element names through HLR?**  Yes, and more
   cheaply than assumed.  Both OCCT HLR paths expose the source edge of
   every projected edge through public API: the exact path through
   `HLRBRep_Data::EDataArray()` / `EdgeMap()`, the polygon path through
   the `TopoDS_Shape&` that `HLRBRep_PolyAlgo::Hide` / `Show` fill in.
   No geometric correlation, no second HLR pass, no OCCT patch.
   Section 3.5.


## 2. Pre-task: store the base shape with every element reference

### 2.1 The recovery ladder that exists today

Three layers, in the order they are consulted:

| layer | mechanism | where |
| --- | --- | --- |
| 1. mapped name | `GeoFeature::resolveElement` against the stored shadow name | `PropertyLinks.cpp:353` |
| 2. geometric search | `searchElementCache` -> `TopoShape::searchSubShape` against a snapshot of the old sub-shape | `PropertyLinks.cpp:382` |
| 3. history guess | `Part::Feature::getRelatedElements` -> `getElementSource`, longest common ancestry in the element map | offered by PartDesign's Gui, not applied automatically |

Layer 1 is the topological naming fix and does the overwhelming
majority of the work.  Layer 3 is only ever *offered*: PartDesign's
`Utils.cpp:1727` warns and populates a list of candidates for the user
to pick from.  Layer 2 is the interesting one, because it is fully
automatic and it is the only layer that consults geometry.

### 2.2 Where the base-shape reference already lives

Layer 2's evidence is gathered in `Feature::onBeforeChange`
(`PartFeature.cpp:1226`).  Before the `Shape` property changes, the
feature walks every property that holds an element reference to it
(`App::PropertyLinkBase::getElementReferences(this)`), and for each
referenced element it stores the sub-shape:

    res.first->second.shape = propShape->getShape().getSubTopoShape(
            element + (prefix?prefix->size():0), true);

into

    struct Feature::ElementCache {
        TopoShape shape;
        mutable std::vector<std::string> names;
        mutable bool searched;
    };

`searchElementCache` (`PartFeature.cpp:1354`) then runs
`newShape.searchSubShape(it->second.shape, &names, options, tol, atol)`
on the new geometry, and if that finds nothing it falls back to "if the
new shape has exactly one sub-shape of that type, take it".

Four properties of this cache decide the whole design:

- **It is a sub-shape, not a base shape.**  One face, detached from
  everything around it.  There is no adjacency, no neighbourhood, no
  element map, and no way to say what the face was *part of*.
- **It is in memory only.**  No `Save`, no `Restore`, no property.  It
  is rebuilt from scratch on every shape change and dies with the
  document.
- **It is gathered opportunistically.**  Gated on
  `!getDocument()->testStatus(App::Document::Restoring)` and
  `!getDocument()->isPerformingTransaction()`, and it can only see
  referrers whose owning document is loaded.  A cross-document XLink
  from a document that is not open contributes nothing.
- **It is one generation deep.**  The next shape change clears it and
  re-seeds it from whatever the references say *now*, broken or not.

So the base shape is not merely unstored: it was never gathered.  The
snapshot is taken at exactly the right moment and then thrown away along
with all of its context.

### 2.3 What the ladder can and cannot do, measured

Model: a 20x20x20 `Part::Box` cut by a radius-3 `Part::Cylinder`, and a
`Part::Plane` attached `FlatFace` to one face of the `Part::Cut`.  The
reference is broken by replacing the Cut's `Base` with a **new** box
object 25 tall, so the old element's source tag no longer exists and
layer 1 cannot resolve.  Three picks, same edit (`probe_fix3.py`):

| picked face | before | after the edit | outcome |
| --- | --- | --- | --- |
| `Face1` (side) | area 400.00, centre (0, 10, 10) | area 500.00, centre (0, 10, 12.5) | repaired, silently |
| `Face6` (side) | area 400.00, centre (20, 10, 10) | area 500.00, centre (20, 10, 12.5) | repaired, silently |
| `Face3` (top, holds the hole) | area 371.73, centre (10, 10, 20) | -- | **`?Face3`**, reference lost |

Two things to take from this, and they pull in opposite directions.

**The recovery is better than it looks on paper.**  The two side faces
changed area (400 -> 500) and moved their centre, and were still
matched: `searchSubShape` is comparing the underlying surface, not the
bounded face, so a face that grows on a plane that stayed put is found.
That is a good design and it is why this fork's references survive
ordinary editing.

**When it fails, everything is gone at once.**  `Face3` sat on the plane
`z = 20` and the edit moved that plane to `z = 25`.  Layer 1 fails (new
tag), layer 2 fails (different plane), and the end state is:

    <Exception> Attacher.cpp(865): AttachEngine3D: subshape not found Cut.?Face3

a property whose value is the literal string `?Face3`, an exception in
the report view, a `Part::Plane` stranded at its old placement, and
**no surviving description of what `Face3` was**.  It was a 371.73 mm^2
planar face at `z = 20` with a 28.3 mm^2 hole in it one recompute ago.
Nothing in the document says so.

**And the successes are as unaccountable as the failure.**  `Face1` and
`Face6` were repaired with no user-visible event at all -- in that run
not even the `FC_WARN "auto change element reference"` line fired,
because the *indexed* name `Face1` still parsed and layer 1 accepted it.
The reference now points at a face that happens to be correct.  Nothing
in the document records that a substitution happened, what the old
geometry was, or that the user might want to check it.  On a model where
the index happens to land on the wrong face, this is exactly the failure
mode `37cb56d3a0` had to fix in TechDraw's dimension references: a
plausible wrong answer is worse than a visible missing one.

> The pre-task is not "make broken references recoverable".  It is
> "make every repair, successful or not, something the user can see and
> overrule" -- which requires keeping the evidence, which requires
> storing it.

### 2.4 What a stored base shape buys that a stored sub-shape does not

Storing the old *sub-shape* would already fix the durability half.  It
is worth being explicit about why the base shape is the right unit:

- **The element map comes with it** -- *only if it is stored as a shape
  property rather than as a bare content hash.  Corrected in sec 5.2;
  the blob a hash names is geometry alone.*  A stored base shape carries
  its `<ElementMap2>` block, so the old mapped name can be looked up in
  the old map, and layer 3's ancestry walk becomes available across a
  reload rather than only while every ancestor object still exists.
- **Adjacency and neighbourhood.**  "The face between these two edges",
  "the face that shared an edge with the one that is still there" are
  answerable from a base shape and unanswerable from a detached face.
  This is the ingredient that would let `handleNoExactMatch`-style logic
  finally be written -- on both sides of the fork the phase-2 similar
  matching stopped exactly here, for want of context.
- **It is showable.**  The point of the whole exercise: a repair dialog
  that renders the old shape with the picked element highlighted, next
  to the new shape with the proposed element highlighted.
- **It costs the same as the sub-shape, or less.**  This is the part
  that only became true recently -- see below.

### 2.5 Why it is cheap now, measured

The premise, from `docs/SharedShapeStorage.md` sec 12.3: since
2026-08-17 `PropertyPartShape` is an ordinary blob referrer.  Its
geometry is one file addressed by content, skipped when unchanged,
pruned when orphaned, and listed with **every property that refers to
it**.

Measured on the probe model (`probe_ref.py`), a schema-5 save:

    Document.xml                        raw=20572   stored=3095
    blobs/Content.xml                   raw=608     stored=319
    blobs/Box.Shape.brp                 raw=1915    stored=531
    blobs/Box.ShapeMaterial.FCMat       raw=675     stored=329
    blobs/Cut.Shape.brp                 raw=3462    stored=825
    blobs/Cyl.Shape.brp                 raw=962     stored=370
    blobs/Plane.Shape.brp               raw=640     stored=262

and the index:

    <F n="Box.Shape.brp"           h="2ce295d0..." r="2965:Box.Shape"/>
    <F n="Box.ShapeMaterial.FCMat" h="1e39ec8b..." r="2965:Box.ShapeMaterial 2966:Cyl.ShapeMaterial 2967:Cut.ShapeMaterial 2968:Plane.ShapeMaterial"/>
    <F n="Cut.Shape.brp"           h="9e110747..." r="2967:Cut.Shape"/>
    ...

The material line is the proof of the mechanism: **four properties, one
file, one hash, four referrer tokens.**  Multi-referrer content sharing
is not a thing that would have to be built for this; it is what the
store already does, and it is what a link property joining the referrer
list would use unchanged.

The shape property already records its blob by hash, inline:

    <Property name="Shape" type="Part::PropertyPartShape">
      <Part HasherIndex="0" ElementMap="1.15.80001.4"
            hash="9e1107472c246e99c9c300d9a3af445b0e595408"/>
      ...

### 2.6 The format: one attribute

Measured, this is exactly what a link sub-element serializes today
(`probe_ref3.py`, the `Part::Plane`'s `AttachmentSupport`):

    <Property name="AttachmentSupport" type="App::PropertyLinkSubList">
      <LinkSubList count="1">
        <Link obj="Cut" sub="Face1" shadow=";Face1;:H87a,F.Face1"/>
      </LinkSubList>
    </Property>

`obj`, `sub`, `shadow`.  The proposal is one more attribute:

    <Link obj="Cut" sub="Face1" shadow=";Face1;:H87a,F.Face1"
          base="9e1107472c246e99c9c300d9a3af445b0e595408"/>

naming the blob the reference was last successfully resolved against.
An older reader ignores an unknown attribute, so this is additive within
schema 5; it is still gated on schema 5 because that is where the store
exists at all.

`PropertyLinkSub`, `PropertyLinkSubList`, `PropertyXLinkSub` and
`PropertyXLinkSubList` all serialize `<Link .../>` through the same
shape of code, so the attribute lands in one place per class, next to
the existing shadow handling.

The property becomes an `App::BlobReferrerProperty`
(`src/App/FileBlobManager.h:138`) -- the interface `PropertyFileIncluded`
and `PropertyPartShape` already implement -- so that `collectBlobs`
notes the hash and keeps the blob alive, and `assignRestoredBlob`
receives it back.

### 2.7 The cost when it is not free, and the retention policy

The honest accounting.  Recording the hash of a shape the file already
holds is free.  But the case the feature exists for is precisely the
case where the referenced object *has* changed, and then the old blob
has to be **retained** rather than pruned -- content the file would
otherwise not carry.

Three things bound that, and they should be built in from the start:

1. **One generation per (object, property), not per reference.**  Every
   link into `Cut.Shape` shares one base blob.  A model with 200
   references into 20 features retains 20 shapes, not 200.
2. **Retention is conditional on breakage.**  While every reference into
   an object resolves cleanly through layer 1, the base attribute can be
   rewritten to the *current* hash on save, and the old blob falls out
   of the referrer list and is pruned by the existing orphan sweep.  The
   file only grows for objects that actually have an unrepaired or
   recently repaired reference.
3. **A ceiling.**  A per-document cap, and a preference to disable the
   whole thing, so that a pathological history cannot grow a file
   without limit.

Sizing, from the probe: `Cut.Shape.brp` is 3462 raw / 825 stored.  For a
real assembly, `docs/SharedShapeStorage.md` sec 11.4 measured
`scanner.FCStd` at 875496 bytes of collapsible geometry across 36
objects, i.e. tens of kilobytes per retained generation.  That is the
number the ceiling has to be set against, and it is not yet measured for
this feature.

**Open question, not settled here.**  Whether the retained generation
should be the whole base shape or the base shape *pruned to the
neighbourhood* of the referenced elements (the referenced sub-shapes
plus their adjacent faces, as one shell).  Pruning would cut the cost by
an order of magnitude on large parts and keeps everything section 2.4
asked for except the complete element map.  It also costs a new shape
that is not identical to any shape already in the file, which forfeits
the free case of 2.5.  This wants measuring on a real assembly before it
is decided; the build order below does the whole shape first because it
is the one that is provably free while healthy.

### 2.8 What it enables, in order of value

1. **A repair dialog with two pictures.**  Old shape, picked element
   highlighted; new shape, proposed element highlighted; accept, pick
   another, or leave broken.  This is the friendlier reference fix the
   whole pre-task is for, and it is impossible today because the left
   picture does not exist.
2. **A repair record.**  A silent automatic repoint becomes a listed
   change the user can review after a recompute, like a merge conflict
   list.  Section 2.3 showed that today a *successful* repair is as
   invisible as a failed one.
3. **Layer 2, durably.**  The geometric search stops depending on having
   witnessed the change live, so it works after a reload, after an undo,
   through a transaction, and for a cross-document link whose other
   document was closed at the time.
4. **Layer 3, across a reload.**  The old element map travels with the
   old shape, so ancestry matching no longer needs every ancestor object
   to still exist in its old form.
5. **The phase-2 matching neither fork ever wrote.**  Both upstream's
   `findSimilar*` stubs and this fork's `handleNoExactMatch()` stopped
   for want of context about what the old element was part of.

### 2.9 Build order for the pre-task

- **P1. Persist the hash.**  Add `base=` to the four link classes'
  `<Link>` serialization, populate it wherever `shadow` is populated,
  make the properties blob referrers so the blob is kept and restored.
  No behaviour change: nothing reads it yet.  Gate:
  `src/Mod/Test/ShapeStorage.py` extended -- the attribute survives a
  round trip, a healthy save rewrites it to the current hash, an
  orphaned generation is pruned.
- **P2. Feed layer 2 from it.**  `Feature::searchElementCache` falls
  back to the stored base shape when `_elementCache` has no entry, which
  is every path listed in 2.2 as opportunistic.  Still automatic, still
  silent -- but now it works after a reload.  Gate: the `probe_fix3.py`
  matrix, run in a second process against a saved file.
- **P3. Record repairs.**  A per-document list of substitutions made
  during a recompute, with the old and new mapped names and the old base
  hash.  No dialog yet; a Python accessor and a report-view summary.
  This is what turns 2.3's silent success into something checkable.
- **P4. The dialog.**  Two viewports, old and new, driven by P3's list.
- **P5, optional, gated on measurement.**  Neighbourhood pruning (2.7),
  if the retention cost on a real assembly says the whole shape is too
  much.

P1 and P2 are the whole functional change.  P3 and P4 are the part the
user actually sees, and are the reason to do it.


## 3. Main task: TechDraw adopts element names

### 3.1 The finding

    $ grep -rn "ElementMap|MappedName|IndexedName|getElementName" src/Mod/TechDraw/
    (0 matches)

TechDraw has no element-map awareness of any kind.  The module is
`TopoDS_Shape`-based end to end and every identity in it is positional.

### 3.2 Where the map is dropped

`ShapeExtractor::getShapes` (`src/Mod/TechDraw/App/ShapeExtractor.cpp:96`)
is the entry point for all view geometry.  Line 105:

    auto shape = Part::Feature::getShape(l);

`Part::Feature::getShape` returns a `TopoDS_Shape`.  The named variant
`Part::Feature::getTopoShape(obj, ..., noElementMap=false)` is declared
on the next line of `PartFeature.h` and is never called from TechDraw.
The shapes are then rebuilt into a raw `BRep_Builder` compound (line
114), and `getShapesFused` fuses with a bare `BRepAlgoAPI_Fuse` (line
195).

So the element map is discarded before the first projection, in the
first function.  Everything downstream is a consequence of that one
line.

### 3.3 The three positional identity schemes

| what | identity | where | how it breaks |
| --- | --- | --- | --- |
| projected edges | `TopExp_Explorer` order over each HLR category compound, categories appended in a fixed sequence | `GeometryObject.cpp:572` | any model edit, and a `ScrubCount` change, renumbers everything |
| projected vertices | order of first appearance while walking edges, deduplicated by position | `GeometryObject.cpp`, `addGeomFromCompound` | one new edge renumbers every later vertex |
| faces | wires **sorted by area** | `DrawViewPart.cpp:584`, `EdgeWalker::sortWiresBySize` | two faces cross in area and every hatch on them swaps |

`BaseGeom` reserves the slot for a 3D backlink and never fills it:

    int ref3D;                      //obs?
    ...
    int ref3D;                      //obs. never used.

The consumers that persist one of those three schemes:

| holder | stores |
| --- | --- |
| `DrawViewDimension::References2D` | `PropertyLinkSubList` of `"Edge5"` / `"Vertex2"` |
| `DrawViewDimension::References3D` | `PropertyLinkSubList` into the source model -- **could be topo-named today** |
| `DrawHatch::Source`, `DrawGeomHatch::Source` | `PropertyLinkSub` holding `"Face3"` |
| `CosmeticVertex::linkGeom` | an int; the comment reads `//connection to corresponding "geom" Vertex (fragile - index based!)` |
| `GeomFormat::m_geomIndex` | an int; `Cosmetic.h:161`, `//connection to edgeGeom` |
| `CenterLine::m_faces / m_edges / m_verts` | 2D name strings |
| `DrawViewDimExtent::Source` | `PropertyLinkSubList` of 2D edges |

What holds the line today is `DrawViewDimension`'s own `SavedGeometry`
(`Part::PropertyTopoShapeList`) plus `GeometryMatcher` -- the same shape
of idea as `_elementCache` in section 2, built independently inside
TechDraw, and persisted where the core one is not.  It recovers geometry
that did not change.  It cannot follow an element through an edit, and
`handleNoExactMatch()` still carries the comment "this is where we
insert the clever logic".

### 3.4 Sections, which are worse for a structural reason

Two stacked instabilities:

1. **The cut is anonymous.**  `DrawViewSection` cuts with raw
   `BRepAlgoAPI_Cut` (`DrawViewSection.cpp:583`, `593`, `617`), and
   `DrawComplexSection` the same.  Even a named input would come out
   unnamed.  The fork's `TopoShape::makEBoolean("Cut", ...)`, which
   propagates names through the `Generated` / `Modified` history, is
   right there and unused.
2. **The cut face has no name to inherit.**  The section faces are
   *created by* the cut, from `makeCuttingTool`
   (`DrawViewSection.cpp:715`), a prism built ad hoc with no element map
   of its own.  A correct `makEBoolean` would still name the new faces
   off an unnamed tool.

So the cut surface -- the thing users hatch, colour, dimension against,
and which this fork additionally renders into the shaded underlay
(`ShadedUnderlay.cpp`) and streams to the browser (`PageFeed.cpp`,
`PageServe.cpp`) -- is identified by `alignSectionFaces()` output order
into `m_sectionTopoDSFaces`, then by area-sorted index in
`m_tdSectionFaces`.  Nudge `SectionOrigin` and every reference to it is
a coin flip.  `DrawViewDetail` inherits the same problem through
`BaseView`, and `UsePreviousCut` chains it further.

This is where the fork has invested most
(`docs/TechDrawPortAndSection.md` sec 31: stored `m_cutFrame` /
`m_preparedFrame`, prepared-space capture, per-face colours), which
makes the unstable identity underneath it the load-bearing weakness.

### 3.5 The enabler: HLR already knows the source edge

This is the result that decides the cost of the whole main task, and it
is better than a geometric correlation scheme.

*Status 2026-09-17:* the traversal below now lives in the Part module as
`Part::HLRProjector` (`src/Mod/Part/App/HLRProjector.*`, with
`TopoShape::makEHLR` and `Shape.makeHLR`), which also resolves a
silhouette to its source face through `HLRTopoBRep_Data` and names the
result with an element map (op code `HLR`). The Sketcher's external face
path uses it (`docs/SketcherPort.md`, "External faces"); TechDraw's private
copy of the same traversal in `GeometryObject.cpp` is the next caller to
move over.

**Exact HLR.**  `HLRBRep_HLRToShape::InternalCompound` walks
`DS->EDataArray()` by index `ie` and calls a private `DrawEdge(...)` per
edge.  `HLRBRep_Data::EDataArray()` is indexed identically to
`HLRBRep_Data::EdgeMap()`, an `NCollection_IndexedMap<TopoDS_Shape>` of
the **original** edges.  Every accessor that traversal needs is public:

    HLRBRep_Algo : public HLRBRep_InternalAlgo   // DataStructure() is public
    HLRBRep_Data::EDataArray()   EdgeMap()   FaceMap()
    HLRBRep_Data::NbEdges()      NbFaces()   Projector()
    HLRBRep::MakeEdge()          MakeEdge3d()
    HLRAlgo_EdgeIterator

So an in-tree reimplementation of `InternalCompound` -- roughly 60 lines
-- emits the same edges in the same order **and reports `ie` with each
one**.  That is the source edge, exactly, with no matching heuristic and
no second HLR pass.

**Polygon HLR** (the `CoarseView` path, `HLRBRep_PolyAlgo`) is easier
still: its public iteration hands back the source shape directly.

    HLRAlgo_BiPoint::PointsT& Hide(HLRAlgo_EdgeStatus& status,
                                   TopoDS_Shape& S, bool& reg1, ...);
    HLRAlgo_BiPoint::PointsT& Show(TopoDS_Shape& S, bool& reg1, ...);

`S` is filled with the shape the segment came from, per segment.

**Trap, if the two-pass route is taken instead.**  `DrawEdge`'s only
difference under `In3d` is `MakeEdge` versus `MakeEdge3d` over the same
`(sta, end)` interval -- the selection and the traversal are identical,
so 2D and 3D results do correspond one to one.  But both makers are
followed by `if (!E.IsNull())`, and nothing guarantees the two agree on
nullity for a degenerate interval.  The reimplemented traversal has no
such gap and is the better route.

### 3.6 Build order for the main task

Each step is independently useful and independently shippable.

**T0. Stop discarding the map.**  `ShapeExtractor` returns
`Part::TopoShape` via `getTopoShape`; fuse with `makEBoolean("Fuse")`;
compound with `makECompound`.  Purely mechanical, nothing consumes the
names yet.  This alone makes `References3D` topo-nameable, which fixes
the model-facing half of dimension breakage with no HLR work at all.

**T1. Name the cutting tool, then cut with names.**  Give
`makeCuttingTool()` a hand-built element map with fixed names for its
faces (`SectionPlane` and the box sides), then switch the cut to
`makEBoolean("Cut")`.  Section faces then carry stable generated names
derived from `SectionPlane`, and stay stable as the model changes.  Same
treatment in `DrawComplexSection`.  **This is the highest-value change
for section stability and it is contained to two functions.**

**T2. Carry names through HLR.**  Replace the `HLRToShape` calls in
`GeometryObject::projectShape` with the in-tree traversal of 3.5, so
every `BaseGeom` learns its source element.  Store the mapped name on
`BaseGeom` where `ref3D` is reserved.

Give each projected edge a stable name of the form

    <sourceElementName>;HLR:<class>:<ordinal>

where `class` is the `edgeClass` plus visibility and `ordinal`
disambiguates one source edge split into several fragments by hiding.
Vertices name from the edges that meet at them; faces name from the
wires that bound them, which kills the area-sort fragility outright.

**T3. Migrate the consumers.**  `References2D`, `DrawHatch::Source`,
`GeomFormat::m_geomIndex`, `CenterLine`, `CosmeticVertex::linkGeom`.
Each keeps its index for display and gains a stable name for
persistence.  `SavedGeometry` is demoted to the fallback it should be --
consulted when the name resolves to nothing, not as the primary
mechanism.  The recovery ladder becomes *name -> exact geometry ->
similar geometry -> ask the user*, where today it is only the middle
rung, and the last rung is what the pre-task's stored base shape makes
possible.

**Sequencing.**  T0 and T1 together are a small self-contained change
that fixes sections without touching HLR.  Do those first and measure
before committing to T2, which is the only genuinely hard piece.

### 3.7 Traps, all of them already paid for in this tree

- `DrawViewDimension::execute()` runs while the document is still
  restoring, against a view that has not projected yet.  A migration
  keyed on "have I run once" spends its one chance on that pass and does
  nothing.  A migration must report whether it could *evaluate*, not
  merely whether it ran.  (`docs/TechDrawPortAndSection.md` sec 34.3.)
- `SavedGeometry`'s stored frame negates Y relative to the Python
  accessors.  A test built from accessor values without the flip matches
  neither frame and looks like a broken fix.  (Sec 34.2.)
- Every ported or reworked Gui item has a second renderer to satisfy:
  the Qt `QGraphicsScene` and the vg feed through `PageFeed`, and
  `DrawViewPart` descendants additionally answer to `ShadedUnderlay`.
  T3 touches only App-tier persistence and should stay clear of both; if
  it does not, that is the signal the change has grown.  (Sec 33.6.)
- Scaling a page view past about 3x segfaults in the Qt FreeType raster
  engine on the stock template's `line-height:0%` text.  Hide the
  template item first, and only after the page view exists.  (Sec 35.3.)


## 4. What is not measured

Stated plainly so the next session does not mistake an argument for a
result.

- **The retention cost of the pre-task on a real model.**  Section 2.7
  reasons about it from `scanner.FCStd`'s collapsible-geometry figure
  and sizes one generation at 3462 bytes on a toy.  Nobody has taken a
  real assembly, broken references in it, and weighed the file.  That
  measurement decides P5 (neighbourhood pruning) and the ceiling.
- **Whether layer 2 fed from a stored base shape repairs materially more
  than layer 2 fed from `_elementCache`.**  Section 2.3 shows it repairs
  *later* (after a reload), which is the durability claim.  It does not
  show it repairs *better*, because P2 does not change the matching
  algorithm -- only P3 onward does.
- **Everything in section 3 past T1.**  The HLR provenance result is
  read from OCCT source and header signatures, not run.  No projected
  edge has yet been traced back to a source element in this tree.
- **The `?Face3` failure is a constructed one.**  Replacing a feature's
  `Base` with a brand-new object is the bluntest possible way to defeat
  layer 1.  It is a real user action -- delete and rebuild a base
  feature -- but the *frequency* of unrepaired references in ordinary
  editing is not measured, and the two side faces in the same run were
  repaired correctly.  The case for the pre-task rests on
  accountability, not on a claimed failure rate.


## 5. The pre-task, planned (2026-09-05)

Written after reading the storage path end to end and measuring four
things sections 2 and 4 left open.  **Three of section 2's claims do not
survive that reading and are corrected in 5.2.**  The plan that follows
is what is left standing.

### 5.1 Scope: what this builds, and what waits

**In scope.** Make the evidence for an element reference *durable* --
persist the snapshot that `Feature::onBeforeChange` already takes, and
feed the existing layer-2 search from it after a reload.  No new matching
algorithm, no new user interface, no change to what a repair decides.

**Deferred, by user ruling (2026-09-05), until upstream's `PartDesign`
and `Sketcher` are merged**: the repair record (old sec 2.9 P3) and the
repair dialog (P4).  This is the right ordering rather than only a
convenient one.  The candidate-list UI a repair dialog would extend
already exists, in `src/Mod/PartDesign/Gui/Utils.cpp:1727` -- it warns on
a missing element reference and offers `getRelatedElements` candidates in
a tree widget -- and that file is squarely inside the merge.  Building a
second UI on top of it now buys rework.

So the pre-task ships as **storage plus the search that consumes it**,
and stops there.  What it produces is not user-visible; what it produces
is a document that still knows what a broken reference used to point at,
which is the thing the deferred UI cannot be built without.

**The failure it exists to fix, stated exactly.**  Section 2.3 measured
`?Face3` and called it "geometry gone forever".  Reading the gathering
code says why, and it is sharper than that:

    if(!element || !element[0] || Data::hasMissingElement(element))
        continue;                       // PartFeature.cpp:1273

A reference that is *already* broken is skipped when the cache is seeded.
So the ladder has no second chance: the snapshot is only ever taken while
the reference is healthy, it lives in memory, and if the document is
saved between the break and the next edit, the last healthy snapshot is
gone.  Layer 2 does not fail because it is weak.  It fails because its
evidence was never written down.

### 5.2 Three corrections from writing the plan

**Correction 1: a content hash names geometry, not names.**  Section 2.4
said "the element map comes with it".  It does not.
`PropertyPartShape::Save` (`PropertyTopoShape.cpp:1060`) writes the
geometry as the blob referenced by `hash=`, and then writes the element
map *separately* -- `_Shape.Save(writer)`, which lands as the
`<ElementMap2>` block inline in `Document.xml` or as a `.Map` file -- and
the string hasher separately again, as a `.Table`.  Three artifacts, one
of which is content-addressed.  Storing a hash and nothing else buys
geometry with no names on it.

Measured on the six-cut plate (`probe_prune.py`), deflated:

| artifact | deflate |
| --- | --- |
| one whole base shape (`Cut5`, 12 faces) | 1613 |
| all six element maps in `Document.xml` | 1240 |

so the map is the same order of cost as the geometry, not a rounding
error on it.  This is what decides 5.3: the stored thing has to be a
*shape property*, which writes all three, and not a bare hash.

**Correction 2: hash plus two more attributes, if the hash route were
taken anyway.**  A blob is not a self-contained shape.  The top-level
location is stripped out of the file and written as `loc=` on `<Part/>`
(`docs/SharedShapeStorage.md` sec 12.2), and a shape that borrows a
congruent instance's file carries `motion=` as well
(`PropertyTopoShape.cpp:1088`).  Only the borrowing *plan* is recoverable
from the file itself -- `serveFromBlob` reads it back out of the parsed
content on purpose.  So "hash -> shape" is not a function; it is
`(hash, loc, motion) -> shape`.

**Correction 3: neighbourhood pruning is dead.**  Section 2.7 left open
whether to store the whole base shape or prune it to the referenced
element plus its adjacent faces, and guessed pruning would save "an order
of magnitude".  Measured, on the same shape, deflated:

| stored | deflate | share |
| --- | --- | --- |
| whole base shape (12 faces) | 1613 | 100% |
| neighbourhood: picked face + the 4 sharing an edge | 1286 | **80%** |
| the picked face alone | 432 | 27% |

Pruning to a neighbourhood saves **20%**, not an order of magnitude.
BRep size is dominated by surfaces and curves, and on any part small
enough to care about, the neighbourhood of a face *is* most of the part.
It is the worst of both options: it keeps most of the cost, and because
the pruned shape is content nothing else in the file holds, it forfeits
the free-when-healthy sharing that the whole shape gets.

**Drop P5.**  The real choice is binary -- the whole shape (free when
healthy, shareable, carries context) or the bare sub-shape (27%, never
free, no context).  The whole shape wins on the common path, which is the
one that runs every save.

### 5.3 Where the shape goes: the layering question, answered

The question is real: `App::PropertyLinkSub` and its three siblings live
in `src/App/`, which cannot see `Part::TopoShape`.

**An App-space geometry property is possible.**  This is worth stating
because it is the option the question implies, and it is not blocked by
layering:

- `App::PropertyComplexGeoData` (`src/App/PropertyGeo.h:611`) is
  App-space and abstract over `Data::ComplexGeoData`, which is itself an
  App-space `Base::Persistence`.  App can `Save` and `Restore` one
  polymorphically without naming the concrete type.
- `Part::PropertyPartShape` is registered --
  `TYPESYSTEM_SOURCE(Part::PropertyPartShape, App::PropertyComplexGeoData)`
  (`PropertyTopoShape.cpp:83`) -- so
  `Base::Type::fromName("Part::PropertyPartShape").createInstance()`
  builds one from App with no link against Part.  That is not a
  speculative trick: it is exactly what
  `DynamicProperty::_addDynamicProperty` already does
  (`DynamicProperty.cpp:184`, `198`) for every dynamic property in every
  document.
- The blob layer is entirely App-space and content-opaque: a
  `FileBlobHandle` is hash, path and size (`FileBlobManager.h:112`), so
  App can *retain* content it cannot parse, and holding the handle is
  itself the retention -- a blob is deleted only when the last handle
  goes away.

So the reason not to put it on the link property is **cost, not
layering**, and the cost is measured.

**The unit is the feature, not the reference.**  Measured on the six-cut
plate (`probe_gen2.py`), against a 17187-byte file:

| retention policy | added, deflated | file growth |
| --- | --- | --- |
| one generation for the one referenced feature | 1572 | **+9.1%** |
| one generation for every feature | 8422 | **+48.9%** |

A base shape is one shape per *feature* however many references point
into it, and the existing `_elementCache` is already keyed that way -- a
map on the feature, from element name to sub-shape.  Storing it on the
link property instead multiplies a 9.1% item by the reference count, and
makes it worse than that: `PropertyPartShape::Copy()`
(`PropertyTopoShape.cpp:984`) copies `_Shape` and `_Ver` and **not**
`_blob`, so every per-reference copy re-serializes and none of them share
the free hash.

**Decision: the snapshot is persisted on the referenced feature, in Part
space, as a shape property.**  Concretely, `Part::Feature` gains

    PropertyPartShape  _SavedShape;      // hidden, the previous generation
    PropertyStringList _SavedElements;   // the element names it was taken for

This is the persistent form of `_elementCache`, put where `TopoShape` is
in scope and where the cache already lives.

**And App needs almost nothing**, because the seam is already there.
`GeoFeature::searchElementCache` is declared virtual in App
(`src/App/GeoFeature.h:176`), returns nothing in the base
(`GeoFeature.cpp:272`), and is implemented in `Part::Feature`.  The one
consumer, `PropertyLinkBase::_updateElementReference`
(`PropertyLinks.cpp:382`), calls it through that virtual and does not
know or care where the evidence came from.  Persist the cache behind the
virtual and every App-side caller gets the durable version with no App
change at all.

The single App-side addition is a **trigger**, not a store -- see step
S3.

### 5.4 What is stored, and when it is not

`_SavedShape` is the *previous* generation, and the invariant is what
keeps the file from growing:

> `_SavedShape` is empty whenever every recorded reference into this
> feature resolves.  It holds geometry only while at least one is
> missing.

Which gives three cases:

| state | what is written |
| --- | --- |
| all references healthy | nothing -- the property is empty |
| a reference just broke | the previous shape; its geometry is a blob the file **already holds** if the feature has not been saved since, otherwise one new blob |
| broken and still broken | the same previous shape, unchanged, so the blob is skipped by the existing content hash and costs no write |

The healthy case is the one that runs on every save of every document
that is working correctly, and it costs nothing.  The 9.1% is what a
document pays *per feature that currently has a broken reference*, which
is the case the feature exists for.

**No ceiling is needed after all.**  Section 2.7 asked for a per-document
cap.  With one generation per feature and retention conditional on an
unresolved reference, the worst case is bounded by the number of features
that simultaneously have broken references -- and a document in that
state has a bigger problem than its size.  A preference to disable the
whole thing is still worth having; a cap is not.

### 5.5 Build steps

**S1. Persist the snapshot.**  Add `_SavedShape` and `_SavedElements` to
`Part::Feature`, both `Prop_Hidden`.  Populate them at the end of the
existing gathering block in `Feature::onBeforeChange`
(`PartFeature.cpp:1226`) -- the snapshot is already taken there, this
only writes it down.  Clear both whenever every entry in
`_SavedElements` resolves against the current `Shape`.

No behaviour change: nothing reads them yet.  This step is measurable on
its own -- a healthy document must save byte-identically to what it saves
today.

**S2. Feed layer 2 from it.**  `Feature::searchElementCache`
(`PartFeature.cpp:1354`) currently returns `none` when `_elementCache`
has no entry for the element.  Make that the point where it falls back to
`_SavedShape`: take the sub-shape named by the element out of the saved
shape, and run the same `searchSubShape` against the live one.

The matching is unchanged.  What changes is that it now also works in
every situation 2.2 listed as opportunistic -- after a reload, inside a
transaction, during an undo, and for a reference whose break happened in
a session that has since ended.

**S3. Fire the search after a restore.**  Storage is inert without this,
and it is the one App-side change.  `GeoFeature::onDocumentRestored`
(`GeoFeature.cpp:265`) already recomputes `_ElementMapVersion`; add: if
this feature has a saved snapshot and any reference into it is missing,
call `PropertyLinkBase::updateElementReferences(this, false)`.

That lands in `_updateElementReference` with `feature == geo` and
`missing == true`, which is exactly the gate the existing recovery
already waits behind (`PropertyLinks.cpp:373`).  Nothing new is invented;
a path that could never fire for a document reopened broken now can.

**S4. A Python accessor, and nothing more.**  Expose the saved element
names and whether a snapshot is held, so the deferred UI has something to
build on and so the gates below can be written.  No dialog, no report
view, no automatic repoint that does not already happen.

Deferred to the `PartDesign` / `Sketcher` merge: the repair record and
the two-picture dialog (old sec 2.9 P3 and P4).

### 5.6 Traps

- **A second `PropertyPartShape` on `Part::Feature` is visited by
  everything that walks shapes.**  The blob owner table, the render
  cache, the store, the property editor and every `getPropertyOfGeometry`
  consumer.  `getPropertyOfGeometry()` returns `&Shape` and
  `onBeforeChange` tests `prop == &Shape`, so the geometry paths are
  safe, but this wants checking against the borrowing and dedup passes of
  `docs/SharedShapeStorage.md` sec 12.12 to 12.14 before it is called
  done -- a saved generation must not become a *source* other shapes
  borrow from, or dropping it later invalidates them.
- **`Copy()` drops the blob.**  `PropertyPartShape::Copy()` carries
  `_Shape` and `_Ver` only.  Assigning the saved shape by copying the
  live property therefore re-serializes rather than sharing the file.
  Take the handle deliberately (`getBlob()`, `PropertyTopoShape.h:148`)
  or accept the extra write and say so.
- **`onBeforeChange` is skipped while restoring and inside a
  transaction.**  Those gates are why the cache is missing exactly when
  it is most wanted, and S1 must not simply inherit them -- but neither
  may it write a snapshot *during* a restore, which would overwrite the
  one being restored.  The condition is "the shape is changing for a
  reason other than being loaded", and it is not the same condition the
  existing block uses.
- **An already-broken reference is skipped by the gathering loop**
  (`PartFeature.cpp:1273`).  That is correct and must stay: the snapshot
  worth keeping is the last *healthy* one, and re-seeding from a broken
  reference would overwrite it with nothing.
- **Do not let S3 turn a restore into a recompute.**  The recovery path
  calls `aboutToSetValue` / `hasSetValue` on link properties.  Firing it
  from `onDocumentRestored` must leave the document not-touched, the way
  `serveFromBlob` takes care to (`PropertyTopoShape.cpp:657`,
  `purgeTouched`).

### 5.7 Gates

Extend `src/Mod/Test/ShapeStorage.py`, run with
`FreeCADCmd -t ShapeStorage`:

1. A document with only healthy references saves byte-identically to a
   build without the feature.
2. Break a reference, save, reopen in a second process: the saved shape
   is present and `_SavedElements` names the element that broke.
3. The same document, reopened: the reference is recovered where the
   geometry still exists -- the `probe_fix3.py` matrix, moved into the
   suite and run across a save boundary rather than in one session.
4. `Face3` of that matrix -- the case that legitimately cannot be
   recovered -- still reports missing, and does **not** get silently
   repointed at a plausible neighbour.
5. Repair the reference by hand, save: the saved shape is dropped and the
   file returns to its healthy size.
6. Undo across the break: the snapshot is not clobbered by the
   transaction the existing gate excludes.

Item 4 is the one that matters most.  The whole argument of section 2.3
is that a plausible wrong answer is worse than a visible missing one, and
a change that makes recovery reach further is exactly the kind that
regresses it.

### 5.8 What this still does not measure

- **Whether persisted layer 2 repairs materially more than live layer
  2.**  It repairs *later*, which is the whole claim and is what gate 3
  tests.  It does not repair *better*: S2 changes when the search runs,
  not how it matches.
- **The cost on a real assembly.**  9.1% is one feature in a 17 KB
  synthetic file.  `docs/SharedShapeStorage.md` sec 11.4's
  `scanner.FCStd` is the model to weigh this against, and nobody has.
- **How often references actually break in ordinary editing.**  Still
  unmeasured, still the honest limit on the case for this work, and still
  the reason it is scoped as accountability rather than as a fix.


## 6. Where to resume (written 2026-09-05, end of session)

*As of 7.11 the first step, V1, is built; the rest of this paragraph
predates it.*  Nothing in this document is built.  Sections 1 to 4 are
the survey, section 5 is the plan for the pre-task, and this section is
what a next session needs that neither of them says.

### 6.1 State of play

| | state |
| --- | --- |
| pre-task (sec 5, re-planned in sec 7) | **V1 and V2 built** (7.11, 7.12); V3 onward not started |
| main task, TechDraw (sec 3) | surveyed, sequenced, **not started** |
| measurements | done; all numbers in secs 2.3, 2.5, 5.2, 5.3 are real |
| probe scripts | session scratchpad only, **will be gone** -- recipes in 6.3 |

**Settled, do not relitigate:**

- The snapshot is persisted **on the referenced feature in Part space**,
  as a shape property, not on the App-space link property (5.3).  An
  App-space property was checked and is possible; the reason against it
  is the measured per-reference cost, not layering.
- The stored unit is **the whole base shape**, not a neighbourhood and
  not the bare sub-shape (5.2, correction 3).
- Retention is **conditional on an unresolved reference**; the healthy
  case writes nothing (5.4).  No per-document ceiling.
- **No user-facing repair UI** until upstream's `PartDesign` and
  `Sketcher` are merged (5.1).

### 6.2 Open decisions the next session has to make

These are genuinely open.  Each carries a recommendation, and the
recommendation is not a decision.

**D1. What `_SavedElements` stores -- indexed names, mapped names, or
both.**  This is the one that can quietly defeat the whole feature.  If
the saved element names are *indexed* (`Face3`), they are positional in
the saved shape exactly as they were in the live one, and a saved
snapshot whose map is not consulted has bought nothing over storing the
sub-shape.  The gathering loop keys `_elementCache` on whatever
`Data::findElementName(sub)` returns for each link, which is whichever
form the link holds.

*Recommendation:* store the pair, the way `ShadowSub` already does --
mapped name first, indexed second -- so a lookup can go through the
saved shape's element map and fall back to position only when the map is
absent.  It also makes the saved property self-describing when read by
hand.

**D2. The Sketcher prefix.**  `Part::Feature` supports more than one
geometry property through `registerElementCache(prefix, prop)`, and
`SketchObject` uses it: `registerElementCache(internalPrefix(),
&InternalShape)` (`src/Mod/Sketcher/App/SketchObject.cpp:229`).  A single
`_SavedShape` covers `Shape` only, and references into `InternalShape`
would get nothing.

*Recommendation:* scope S1 to the primary `Shape` and say so in the
commit.  `Sketcher` is one of the two modules in the coming merge, so a
prefix-aware version built now is a version built against code that is
about to move.  Revisit it as part of that merge.

**D3. Where the persistence decision is taken: `onBeforeChange` or
`beforeSave`.**  Trap 3 of 5.6 is that `onBeforeChange` is gated off
during restore and during a transaction, which is exactly when the cache
is most wanted -- but writing a snapshot *during* a restore would
overwrite the one being restored.

*Recommendation:* leave the gathering in `onBeforeChange` completely
unchanged, and take the persistence decision at `beforeSave`: if the
in-memory cache holds a generation, write it; if it does not and
`_SavedShape` already holds one whose references are still missing, keep
what is there.  That makes S1 the safest possible change -- it adds a
writer and touches no existing condition.

*The cost of that recommendation, stated:* a break that happens entirely
inside a transaction is never seeded into the cache in the first place,
so no amount of save-time persistence recovers it.  Undo/redo-mediated
breaks stay unprotected.  Accepting that is the price of not touching
the gathering gates in the first commit; widening them is a separate,
riskier change and should be its own step with its own gate.

**D4. Other `GeoFeature` kinds.**  Mesh and Fem carry element references
too and would get nothing from a `Part::Feature` property.

*Recommendation:* leave them.  `searchElementCache` is virtual on
`App::GeoFeature`, so each kind can implement its own storage later
without any of this changing.

**D5. Whether the saved shape's hasher and map version need handling of
their own.**  `PropertyPartShape::Save` registers its hasher through
`owner->getDocument()->addStringHasher(...)` and writes an
`ElementMap="<version>"` attribute.  A second shape property on the same
object goes through the same path with a *different* generation's map.

*Recommendation:* verify before building, not after.  The specific
question is whether two shape properties on one object can carry element
maps of different vintages without `GeoFeature::updateElementReference`'s
version comparison (`GeoFeature.cpp:243`) reading the wrong one --
`getElementMapVersion(prop)` takes the property, so it probably behaves,
but "probably" is not what this should rest on.

### 6.3 Reproducing the measurements

The probe scripts are in the session scratchpad and are not committed,
per the `DrawBrokenView` convention.  The recipes are short enough to
restate, and every number in this document comes from one of these three
models under
`.conda\run.cmd build\win-relwithdebinfo-801\bin\FreeCADCmd.exe <script>`:

**Model A -- the reference-recovery matrix** (secs 2.3, 2.6; `probe_ref3.py`,
`probe_fix3.py`).  `Part::Box` 20x20x20; `Part::Cylinder` r=3 h=40 at
(10, 10, -10); `Part::Cut` of the two; a `Part::Plane` with
`AttachmentSupport = [(cut, ("FaceN",))]` and `MapMode = "FlatFace"`.
Break it by assigning `cut.Base` a **brand new** `Part::Box` of a
different height -- a new object is what defeats layer 1, changing the
existing box is not.  Read the outcome from
`pl.AttachmentSupport[0][1]`.  Save with
`doc.SaveSchemaVersion = 5` and read `Document.xml` out of the zip for
the `<Link .../>` and `<Part .../>` serialization.

**Model B -- the retention cost** (sec 5.3; `probe_gen2.py`).  A
120x80x10 `Part::Box` plate, then six `Part::Cylinder` r=4 h=30 at
(15 + 18i, 40, -10), each consumed by a `Part::Cut` chained onto the
previous.  Attach a `Part::Plane` to `Face1` of the last cut.  Save at
schema 5, then set `plate.Height = 14` and save again.  Compare
`compress_size` of `blobs/*.brp` across the two saves, and the count of
distinct `hash="..."` in `Document.xml`.

*Do not use a `Part::Fillet` in this model.*  The first attempt did, with
`Edges` built from `range(len(shape.Edges))`, and it failed with "There
are no suitable edges for chamfer or fillet" -- leaving a null shape and
a 154-byte blob that looked like a real measurement.

**Model C -- pruning and map cost** (sec 5.2, correction 3;
`probe_prune.py`).  Model B without the plane.  Export three shapes to
BRep and deflate each with `zlib.compress(data, 6)`: the whole shape;
the picked face; and a `Part::Compound` of the picked face plus every
face sharing an edge with it (compare by `Edge.hashCode()`).  For the map
cost, regex `<ElementMap2 count="\d+">(.*?)</ElementMap2>` out of
`Document.xml` and deflate the concatenated bodies.

### 6.4 First action next session

*Superseded by section 7.10 on the same day: S1 as planned here would
have failed gate 1, for the reason 7.1 gives.  Kept as written.*

Build **S1** (5.5) under decisions D1 and D3 as recommended: add
`_SavedShape` and `_SavedElements` to `Part::Feature`, populate them at
`beforeSave` from the existing untouched `_elementCache`, clear them when
every saved element resolves against the current `Shape`.

The gate to write first is **gate 1** of 5.7 -- a healthy document saves
byte-identically to what it saves today.  It is the cheapest thing to get
wrong and the cheapest to check, and until it passes there is no point
running the rest.

Then D5's verification, before S2 touches anything.


## 7. The pre-task, re-planned: versioned base shapes on the feature (2026-09-05)

Written the same day as section 5, after the user's rethink of where the
snapshot lives and how many of them there are.  Everything section 5
measured still holds and is reused; what changes is the *shape* of the
storage and the *unit* of retention.  Where this section and sections 5
or 6 disagree, this section wins.

### 7.1 Three moves, and what each one buys

**Move 1: the snapshot is a dynamic property, not a member.**  Section
5.3 gave `Part::Feature` a static `PropertyPartShape _SavedShape`.  That
would have failed its own gate 1 by construction: `PropertyPartShape`
does not opt into the shared-default elision
(`Property::canShareDefault()` is false for it, and `PropertyContainer::
Save` at `PropertyContainer.cpp:343` writes every property that does not
opt in), so an *empty* `_SavedShape` is still a `<Property>` element with
an empty `<Part>` in every object of every document -- 17743 of them on
`MiSTer_imported.FCStd`.  A dynamic property exists only on a feature
that currently holds a generation, so a healthy document is byte-for-byte
what it is today and gate 1 becomes trivially true.  The machinery is
the one `ShapeContents` already uses on the same class
(`getShapeContentsProperty(bool force)`, `PartFeature.cpp:1513`):
`addDynamicProperty` with a `Part::PropertyPartShape` type name, created
by `Base::Type::createInstance` (`DynamicProperty.cpp:198`), removed
through the `Feature::removeDynamicProperty` override
(`PartFeature.cpp:1478`), recorded in the undo transaction
(`Transactions.cpp:486`, `:508`), and restored by type name with no code
on the reading side (`PropertyContainer.cpp:625`).

**Move 2: one generation per referrer, not one per feature.**  Section
5.4's single previous generation loses evidence on the second edit.
Walk it:

| time | event | single generation (sec 5) | per referrer (this section) |
| --- | --- | --- | --- |
| t0 | shape S0; A and B resolve | -- | -- |
| t1 | S0 -> S1; A survives, B breaks | holds S0 for B | holds S0 for B |
| t2 | S1 -> S2; A breaks too | **overwrites with S1**; B's S0 is gone | holds S0 for B and S1 for A |
| t3 | user repairs B | drops S1 (A's evidence, not B's) | drops S0; keeps S1 for A |

The gathering loop skips already-broken references
(`PartFeature.cpp:1273`), so under section 5 the t2 snapshot does not
even re-seed B -- B's evidence is simply overwritten.  Per referrer, a
broken reference keeps the generation it last resolved against until
*that reference* is repaired or removed.  A generation is one shape
however many referrers hold it: it is **reference counted by its
referrers** (user ruling, 2026-09-05), materialized when the first broken
referrer needs it and dropped when the last one lets go.  The count of
retained generations is therefore bounded by the number of distinct
generations that currently have a broken referrer.

**Move 3: the element cache becomes versioned, and layer 2 is a request
against it.**  Today `_elementCache` (`PartFeature.cpp:1206`) is a flat
map from element name to a detached sub-shape, cleared and re-seeded on
every shape change.  It becomes a list of *generations*, newest first,
each holding the whole base shape of that generation.  The one caller,
`PropertyLinkBase::_updateElementReference` calling
`geo->searchElementCache(oldElement)` (`PropertyLinks.cpp:382`), is the
**referencing request**: "find element `X`, as it was when I last
resolved, in the live shape".  The feature answers it from whichever
generation holds `X`, in memory or persisted, and nothing on the App side
changes -- the virtual and its one caller stay as they are.

### 7.2 What identifies a version

Not a counter on the live shape.  A persisted generation counter would be
one more property on every feature, which is exactly what move 1
avoids; and the object revision that exists (`DocumentObject::_revision`,
`DocumentObject.cpp:1046`) is an in-memory counter that is neither
persisted nor tied to the geometry.

Versions are **minted only when a snapshot is taken**, and the snapshot
is anonymous until it is materialized, at which point its name is the
dynamic property's ordinal (`_BaseShape1`, `_BaseShape2`, ...; next
ordinal = max existing + 1, so a reload cannot reuse a name).

The *lookup* key is not the version at all.  It is the element's mapped
name: the whole point of the element map is that an unchanged element
carries the same mapped name from one generation to the next, so "which
generation holds `;Face3;:H87a,F.Face3`" is well defined and the newest
generation holding it is the right answer.  A reference therefore does
not need to remember which version it was resolved against, and the link
properties need no new attribute -- section 2.6's `base=` is dead.
Referrer identity is recorded for **retention only**: it says which
referrer a generation is being kept for, so it can be dropped when that
referrer no longer needs it.

### 7.3 The stored form

One dynamic shape property per retained generation, plus one manifest
that carries the reference count, both on the referenced feature:

    name   _BaseShape<N>
    type   Part::PropertyPartShape
    group  BaseShape
    attr   Prop_Hidden | Prop_ReadOnly | Prop_Output | Prop_NoRecompute

    name   _BaseShapeRefs
    type   App::PropertyMap           -- referrer key -> "_BaseShape<N>"
    group  BaseShape
    attr   Prop_Hidden | Prop_ReadOnly | Prop_Output | Prop_NoRecompute

`Prop_Output` is what keeps a value change from touching the feature
(`DocumentObject.cpp:1035`), and `Prop_NoRecompute` keeps it out of the
recompute; both matter because the property is written from inside
`onChanged(Shape)` during a recompute.

**The manifest is the reference count.**  `_BaseShapeRefs` maps each
referrer key to the generation it holds; a generation's count is the
number of entries naming it, and it is removed in the same step as its
last entry.  The two are created together on the first broken referrer
and removed together when the map empties, so a healthy feature carries
neither.  The key is `Property::getFullName()` of the link property with
the document part dropped when it is this document --
`Plane.AttachmentSupport` for a local referrer, `Other#Bracket.Support`
for an XLink from another file.  Object and property names are
identifiers, so the string is ASCII and unambiguous.

Counting on the feature rather than on the referrer is deliberate: the
decision that costs file size is taken at the feature's `beforeSave`
(7.5), and a feature can only count references it holds a record of.  A
`base=` attribute on the link property would give a referrer in another
document a reference of its own, but the feature could not see it until
that document is loaded, so it would still need this manifest.  It is
withdrawn (7.2).

*One shape, one map, however many referrers.*  A generation shared by
several broken referrers is one file and one element map.  The
per-referrer alternative -- one shape property per referrer, deduplicated
by content -- would have written the element map once per referrer,
roughly 200 bytes deflated per map on the six-cut plate (sec 5.2: 1240
for six maps), and was set aside by the same ruling.

**The blob is transferred, not re-serialized.**  `Feature` is a friend of
`PropertyPartShape` (`PropertyTopoShape.h:151`), and at `onBeforeChange`
the live property still holds the blob of the last save (`_blob`,
`_blobPlan`, `_blobMotion`; `setValue` drops them a moment later through
`dropBlob`).  The snapshot takes the three along with the shape, and the
materialized property is given them back.  So a generation that was saved
as the feature's `Shape` costs the next save **no new geometry** -- the
file is already in the store under its hash, and the version property
only adds a referrer token to it.  That is the "free while the feature
has not been saved since" case of sec 5.4, made exact.  The `Copy()`
trap of sec 5.6 (it carries `_Shape` and `_Ver` and not the blob) is
avoided by never going through `Copy()`.

**A version publishes nothing.**  `makeBlob` (`PropertyTopoShape.cpp:
431`) both borrows sub-shapes from earlier files and publishes its own
into the owner table (`refs.publish`) and the congruence index
(`congruent->add`).  A version property must borrow -- within one object
`Shape` sorts before `_BaseShape*`, so the old generation borrows what it
still shares with the new one -- and must **not** publish: a later object
that borrowed from a generation would have to be rewritten the day that
generation is dropped.  The rewrite exists and is tested
(`testABorrowerIsRewrittenWhenWhatItBorrowedIsGone`), so this is a cost
to avoid rather than a corruption to prevent; a `_publishes` flag on the
property, false for versions, is the whole change.

### 7.4 The versioned cache, in memory

    struct Feature::ShapeVersion {
        TopoShape           shape;      // the whole base shape, a handle
        App::FileBlobHandle blob;       // what the last save wrote for it
        std::string         blobPlan;
        TopLoc_Location     blobMotion;
        PropertyPartShape  *prop;       // the materialized form, or null
        std::set<std::string> referrers;   // the reference count, sec 7.3
        // the search memo, per element, exactly today's ElementCache
        mutable std::map<std::string, std::pair<std::vector<std::string>, bool>> searched;
    };
    std::vector<ShapeVersion> _shapeVersions;   // newest first

`onBeforeChange(Shape)` -- gates unchanged, per D3 -- pushes one
`ShapeVersion` holding the old shape whole instead of extracting a
sub-shape per referenced element, and records the keys of the loaded
referrers whose elements are healthy (the same loop and the same
`hasMissingElement` skip as today).  The sub-shape extraction moves to
search time.  Memory is a handle copy: the old TShape was alive a moment
ago and is released with the generation.

`searchElementCache(element)` walks `_shapeVersions` newest first.  For
a mapped element name it looks the name up in that generation's element
map and takes the indexed sub-shape it names; for an indexed name it
takes the position (that is what today's cache does implicitly, and it
is the D1 answer: the mapped form goes through the map, the indexed
form falls back to position).  Then the same
`newShape.searchSubShape(sub, &names, options, tol, atol)` as today,
the same single-sub-shape fallback, memoized per generation and element.
First generation with a hit answers.  A persisted generation that has
not been parsed yet is parsed on this first request
(`getShape()` -> `ensureRestored()`), which is what keeps the open of a
document holding generations as cheap as today.

The newest generation is always kept in memory whether or not it is
persisted, so the `reverse` regeneration path and the same-session
behaviour are exactly what they are today.

### 7.5 The three flows

**Live change.**  `onBeforeChange(Shape)` snapshots.  `onChanged(Shape)`
runs `GeoFeature::onChanged` -> `updateElementReferences(this)`; every
referrer re-resolves and the missing ones make their request.  *After*
that pass returns, `Feature::onChanged` reconciles: for each in-memory
generation, keep a referrer only if its reference into this feature is
still missing (the same enumeration as the seeding loop); a generation
with referrers left is materialized -- `addDynamicProperty`, the blob
handed over, `setValue(shape)`, its referrers entered in
`_BaseShapeRefs` -- and one whose count is zero is dropped, except the
newest, which stays in memory unpersisted.

**Reload.**  The version properties restore by type name and park their
blobs like any shape.  `Feature::onDocumentRestored` rebuilds
`_shapeVersions` from the properties named `_BaseShape*` and their
referrers from `_BaseShapeRefs`, and then -- this is sec 5.5's S3,
unchanged -- calls `PropertyLinkBase::updateElementReferences(this)` if
any generation is held, so a reference that was broken when the file was
saved makes its request now, against the persisted generation.  A restore
**never writes**: no property is added or removed here, the document
must come up not-touched (the `purgeTouched` discipline of
`serveFromBlob`, `PropertyTopoShape.cpp:685`), and anything the request
repaired is reconciled at the next save.

**Referrer-side change.**  A reference repaired by hand, a referrer
deleted, its document closed: none of these reach the feature.
Reconcile at `Feature::beforeSave`, which already exists
(`PartFeature.cpp:1701`): for each manifest entry, drop it when its key
names a loaded document in which the object or property no longer
exists, or when its reference into this feature now resolves; drop a
generation whose count reaches zero, and the manifest itself when it is
empty.  A key naming a document that is
**not loaded** is kept -- that is the only protection an external
referrer gets, and it is more than today's none.  This is where the
file's size is decided, so it is the right place for the invariant of
sec 5.4 to be enforced: *a generation is written only while some
referrer still needs it.*

### 7.6 Build steps

**V1. The versioned cache, in memory only.**  Replace `ElementCache`
with `ShapeVersion`; `onBeforeChange` snapshots the whole shape;
`searchElementCache` searches generations.  No persistence, no dynamic
property.  Gate: both suites green and the sec 2.3 matrix unchanged in
one session -- this step must be invisible.

**V2. Materialize and drop.**  The reconcile of 7.5 in `onChanged` and
`beforeSave`; the property flags; the blob hand-over; the no-publish
flag; and two fixes the value set needs:
`PropertyPartShape::validateShape` (`PropertyTopoShape.cpp:798`) writes
`InvalidShape` on the owner for *any* shape property, and must be
limited to the owner's geometry property; and
`PropertyPartShape::Restore` (`PropertyTopoShape.cpp:1234`) schedules a
recompute of the owner when a property's element map version is behind
the current one, which a retained generation with an old map would
trigger on every open -- `Feature::checkElementMapVersion(prop, ver)`
answers false for a non-geometry property.  That second fix is D5,
answered.  Gates, extending `src/Mod/Test/ShapeStorage.py`:

1. a healthy document saves byte-identically;
2. break one reference, save, reopen in a second process:
   `_BaseShape1` is present, `_BaseShapeRefs` maps the referrer to it,
   and `blobs/Content.xml` lists the geometry once;
3. break A at one generation and B at the next: two properties, two
   distinct hashes, two manifest entries;
4. break A and B at the same generation: **one** property, two manifest
   entries naming it;
5. repair B by hand, save: B's entry is gone; the generation stays while
   A still names it, and goes when A is repaired too;
6. delete the referrer object, save: its entry is gone, and the
   generation with it if the count reached zero;
7. undo across the break: the version property is removed with it, redo
   brings it back (the transaction records the add).

**V3. Serve after restore.**  The reload flow of 7.5.  Gates: the sec
2.3 matrix across a save boundary (gate 3 of sec 5.7), and gate 4 of
sec 5.7 verbatim -- `Face3` still reports missing and is **not**
repointed at a plausible neighbour.  This gate matters most, for the
reason sec 2.3 gives.

**V4. Nothing to build for Python.**  Sec 5.5's S4 accessor is moot: a
dynamic property is already reachable as `obj._BaseShape1` and the
referrers holding it as `obj._BaseShapeRefs`.  The deferred UI has what
it needs.

**V5, later, each its own commit.**  The Sketcher prefix (D2 stands:
`ShapeVersion` carries the source property, so extending is mechanical,
but it waits for the merge); and widening the `onBeforeChange` gates so
a break inside a transaction is seeded (the cost D3 accepted).

### 7.7 Traps

- **`setValue` on a version property runs the owner's hooks.**
  `validateShape` (fixed in V2), `reTagElementMap` (a no-op here: the
  old shape carries the owner's own tag), `Feature::onChanged` (tests
  `prop == &Shape`, so the placement sync does not fire) and
  `GeoFeature::onChanged` (tests `getPropertyOfGeometry()`, likewise).
  Check each again when V2 lands; the list is from reading, not running.
- **Adding a property inside a recompute.**  `addDynamicProperty` fires
  `signalAppendDynamicProperty` and is transaction-recorded; the property
  editor and the undo stack both see it.  Nothing here may touch the
  feature again or the recompute loops -- `Prop_Output` on the value is
  the guard, and gate 1 of V2 is what proves it held.
- **Restore never writes** (7.5).  Not even a removal: a reconcile
  during restore would land in no transaction and could race the
  deferred blob drain.
- **Copy and paste carries the versions along.**  `copyObject` copies
  dynamic properties, and the manifest keys then name objects in the
  source document.  They are stale, not wrong: the next `beforeSave`
  reconcile drops them, and the generations with them.  If that proves
  noisy, skip `_BaseShape*` and `_BaseShapeRefs` when the owner
  `isExporting()`.
- **The manifest and the shapes must move together.**  Every path that
  adds, drops or restores a generation goes through one helper, so the
  count and the property can never disagree; a manifest entry naming a
  property that does not exist, or a `_BaseShape*` no entry names, is
  dropped at `onDocumentRestored` with a warning.
- **The gathering gates still exclude transactions** -- D3 is accepted
  as is, and a break that happens entirely inside an undo/redo is still
  unprotected until V5.
- **A version's blob must be noted at save or it is pruned.**  Handled by
  `Save()` calling `noteBlob` as for any shape; a version that is
  dropped is simply not written and its file falls out of the index.
- **Two shape properties, one hasher.**  Both the live shape and every
  generation share the document's string hasher, so `addStringHasher`
  returns the same index for all of them and nothing is written twice.
  A generation restored from a file whose hasher was *saved* (the
  `SaveHasher="1"` case) needs checking in V3.

### 7.8 Decisions

Closed by this section:

- **D1**: the lookup is by mapped name through the generation's own
  element map, position as fallback.  Nothing is stored beyond the shape.
- **D3**: gathering gates unchanged; persistence decided after the
  resolve pass and at `beforeSave`.
- **D5**: `Feature::checkElementMapVersion` answers false for a
  non-geometry shape property, so an old generation's map never
  schedules a recompute.
- Sec 5.3's static `_SavedShape` / `_SavedElements` pair is withdrawn;
  sec 2.6's `base=` attribute is withdrawn (7.2).
- **The retention predicate is "missing", and only that** (user ruling,
  2026-09-05).  A reference repaired by the geometry search keeps no
  generation; the silently repaired references of sec 2.3 are not
  evidence this pre-task retains.
- **One generation per shape, reference counted by its referrers**
  (user ruling, 2026-09-05): the manifest form of 7.3, not one property
  per referrer.

Nothing in the pre-task is open.

### 7.9 Not measured, still

Everything sec 5.8 lists.  Plus: the cost of an old generation *not*
sharing TShapes with the new one -- when OCCT rebuilds every sub-shape,
the generation borrows nothing from the live file and the retained
geometry is a whole second file; sec 5.3's 9.1% per broken feature is
the measured bound for that case, per referrer now rather than per
feature.

### 7.10 First action next session

*Done, see 7.11.  Kept as written.*

Build **V1**.  It changes no file on disk, so its gate is the two suites
green plus the sec 2.3 matrix reproduced in one session (recipe in 6.3,
model A).  Then V2 with its gate 1 before anything else.

### 7.11 V1, built (2026-09-05)

`Part::Feature` now keeps `std::vector<ShapeVersion> _shapeVersions`,
newest first, in place of the flat `_elementCache`; the struct is defined
at the top of `PartFeature.cpp` (before the defaulted destructor, which
needs it complete) and holds the source property, its prefix, the whole
shape, the referrer set of 7.3 and the per-element search memo.  The
Sketcher's `registerElementCache` prefix works as before: a generation
belongs to one property, and `shapePropertyOfElement()` is the one place
that maps an element name to its property and prefix.

Four things the code settled that section 7 left implicit:

- **When the gathering gates fail** (restore, undo/redo), no generation
  is taken, exactly as no seed was taken before; but the generations
  that *are* retained for a missing referrer stay, only their memos are
  dropped.  The generation nobody is retained for -- normally the newest
  -- is dropped at the next `onBeforeChange` of its property whether or
  not the gates hold, which is what today's clear did.  Invisible,
  because a retained referrer is never searched again: its indexed name
  carries the `?` marker and the caller skips it.
- **The memo is per generation and is cleared whenever that property's
  live shape changes**, in `onBeforeChange`.  It was implicitly cleared
  before by the cache being rebuilt; with generations that outlive one
  change it has to be explicit, or an older generation would answer
  with names resolved against a shape that is gone.
- **The reconcile of 7.5 runs in memory already**: after
  `GeoFeature::onChanged(Shape)` has re-resolved the references, a
  referrer stays on a generation only if some element of its link
  property into that generation's property is missing now, and an
  emptied generation is dropped unless it is the newest of its property.
  The missing element is attributed to a property by its prefix after
  the `?` marker (`?InternalEdge3` -> `InternalShape`).  V2 adds the
  materialization on top of this; the retention logic does not change.
- **The one caller still passes the indexed name** (`shadow.second`), so
  the lookup is by position today, and a newest-first walk answers from
  the newest generation that has that index.  That is what the flat
  cache did implicitly, and it is exactly right while the caller only
  asks about references that were healthy at the last change.  A
  request that must pick the right generation among several -- the
  deferred repair UI, or a referrer loaded from another document after
  two edits -- should pass the mapped name; `getSubTopoShape` already
  resolves either form, so that is a caller change, not a cache change.
  Noted for V3.
  *Since (2026-10-09, docs/TransactionLogJournal.md sec 31.22):* a request that
  names its referrer is answered only from a generation retained for that
  referrer, where there is one. A merge gives a feature the other branch's
  shape as a generation (`retainElementEvidence`), and a number of that
  shape is a position in it and in no shape of the feature's own. A
  referrer recorded on no generation walks them newest first, as above.

Gate, all in one session, on `build/win-relwithdebinfo-801`:

| check | result |
| --- | --- |
| sec 2.3 matrix (model A, recipe 6.3) | identical: `Face1`, `Face6` repaired silently to area 500.00, `Face3` -> `?Face3` |
| C++ (`ctest -j 6`) | 473 of 473 passed, 1 disabled |
| Python (`FreeCADCmd -t 0`) | 1317 ran, 6 failures + 8 errors, all of them this box: no `yaml` (which also keeps `TestCAMApp` from loading, hence 1317 and not 2628) and no `ply`, CRLF fixtures in the Material clipboard tests, two over-long temp paths in `FileBlobs`, one file lock in FEM.  Every Part, Sketcher, PartDesign, Document, ShapeStorage and FileBlobs geometry case passed. |

Next: **V2**, gate 1 (the byte-identical healthy save) first.

### 7.12 V2, built (2026-09-05)

The retained generations are persisted as planned in 7.3: a dynamic
`Part::PropertyPartShape` named `_BaseShape<N>` per generation and the
`App::PropertyMap _BaseShapeRefs` manifest, both in group `BaseShape`
with `Prop_Hidden | Prop_ReadOnly | Prop_Output | Prop_NoRecompute`,
created by `materializeShapeVersions()` after the `onChanged(Shape)`
reconcile and removed by the reconcile itself when the last entry goes.
`beforeSave` reconciles without materializing.  `onDocumentRestored`
adopts the properties into the list (`adoptShapeVersions()`) without
parsing them -- a generation's shape is read from its property on the
first search request.  The gates of 7.6 are the seven cases of
`BaseShapeCases` in `src/Mod/Test/ShapeStorage.py`.

What the code settled or corrected:

- **The blob is not on the property at `onBeforeChange`.**  7.3 said
  `setValue` drops it "a moment later"; it dropped it a moment *earlier*
  (`dropBlob` ran before `aboutToSetValue`).  The two `setValue`
  overloads now announce first and drop second.  Nothing in between
  reads the blob: the transaction's stand-in is a `Copy()`, which carries
  none.  With that, gate 2 holds as 7.3 promised -- the file the last
  save wrote for the old shape is the generation's file, byte for byte,
  and the index lists that geometry once.
- **The manifest is exact after a save, not after a recompute.**  The
  sec 2.3 side faces are not repaired by the geometry search at all:
  instrumented, `searchSubShape` finds nothing for the grown `Face6`
  (area 400 -> 500), and the reference is marked missing by the cut's
  own resolve pass.  It comes back healthy later in the same recompute,
  when the attach extension re-sets the legacy `Support` from
  `AttachmentSupport`'s raw sub names and "Face6" resolves by position
  in the new shape.  So right after the break the in-memory reconcile
  retains the side-face plane too; the save-time reconcile lets it go.
  Sec 2.3's explanation of *why* the side faces survive ("comparing the
  underlying surface") is therefore wrong: they survive by index, which
  is the plausible-wrong-answer risk sec 2.3 itself names.
- **A referrer holds one generation.**  Because of the above, a referrer
  can be recorded on an older generation from a false alarm and then be
  healthy when the next snapshot is taken.  The snapshot now removes the
  referrers it records from every older generation of the same property,
  and the manifest is built newest first, so the generation a key names
  is always the newest it resolved against.
- **A `Part::Plane` is two referrers**, `AttachmentSupport` and its
  legacy twin `Support`; the manifest carries both keys, and the tests
  expect both.
- **`removeDynamicProperty` of a `_BaseShape<N>` erases its in-memory
  generation**, whoever removes it (the reconcile, or an undo).  A redo
  re-adds the property through the transaction and the next reconcile
  adopts it again.  That is what makes gate 7 hold without the list and
  the properties ever disagreeing.
- **Only `Shape` generations are persisted.**  A generation of a prefixed
  property (the Sketcher's `InternalShape`) is still retained in memory
  and searched, but not materialized -- the persisted name does not say
  which property it belongs to.  V5's item, unchanged.
- The `validateShape` fix is narrower than 7.6 said: it skips the
  `_BaseShape<N>` properties rather than everything but the geometry
  property, so a retained generation is neither fixed nor allowed to
  flag its owner, and the Sketcher's `InternalShape` keeps whatever
  behaviour it had.  D5 is `Feature::checkElementMapVersion`, answering
  false for a `_BaseShape<N>` property.

Gate, on `build/win-relwithdebinfo-801`:

| check | result |
| --- | --- |
| `FreeCADCmd -t ShapeStorage` | 41 of 41, the 7 `BaseShapeCases` included |
| sec 2.3 matrix | identical to 7.11 |
| C++ (`ctest -j 6`) | 473 of 473 passed, 1 disabled |
| Python (`FreeCADCmd -t 0`) | 1324 ran; the same 6 failures + 8 errors as 7.11, all this box's environment, not one new |

*The next step was V3 as of this section; 7.13 re-sequences it.*

### 7.13 Local referrers only, released promptly; foreign referrers keep their own evidence (2026-09-05)

Two rulings from the user after V2, and what was built for the first.

**A retained generation must not be dead weight.**  V2 let go of a
generation at the feature's next shape change or at `beforeSave`; a
reference repaired by hand, a deleted referrer or a repointed one left
the whole old shape in memory until then.  Built:

- `GeoFeature::onElementReferenceReleased(prop)`, a virtual called from
  `PropertyLinkBase::unregisterElementReference()` for every feature the
  property held element references into.  That runs when the property is
  re-set (which is also how the attach extension's `Support` sync
  resolves the sec 2.3 false alarm, 7.12) and when it is destroyed with
  its owner.  A feature that is destroyed takes its registry entry with
  it (`clearElementReferences`, from `~GeoFeature`), so a referrer
  released afterwards finds nothing to notify.
- `Part::Feature` does not decide at the notice -- the property is
  mid-change and may register again a moment later with new content.
  It marks itself pending, per document, and
  `releasePendingShapeVersions()` reconciles (drop only) when
  `App::Application::signalRecomputed` fires for that document, connected
  once in the Part module init.  A pending feature not recomputed again
  is reconciled by `beforeSave` as before.  During a restore or a
  transaction the set is kept for later.
- Effect on the gates: the side-face plane of gate 3 is retained for a
  moment and let go by the end of the same recompute; gates 5 and 6 now
  assert release with no save in between, and a repoint case was added.

**The feature counts only referrers in its own document.**  A referrer
in another document cannot be seen while that document is closed, which
is the normal state of an assembly while its part is edited, so the
feature-side count could never protect it; and keying it by document
name was wrong anyway (a document's runtime name is not stable).  The
`Doc#` key form is gone: `referrerKey` is empty for a foreign referrer,
and the seeding, missing and reconcile loops skip it.  The manifest of a
`_BaseShape<N>` names local `Object.Property` keys and nothing else.

**Foreign referrers: decided, not built.**  The referrer document stores
the evidence itself, as **sub-shapes, not whole shapes**: one
document-wide dynamic `Part::PropertyPartShape` holding a compound of
every foreign sub-shape the document's element references resolved
against, and one `App::PropertyMap` manifest mapping **the full
reference name** to the child's index in the compound, so that each
link property looks its sub-shape up from its own link content.  User
ruling.  Two refinements agreed for the key: the document part is the
XLink's persisted file path (relative to the referring file), not the
runtime document name; and the element part is the indexed name
(`Cut.Face3`), which is what the lookup has in hand after a break and is
kept exact by the refresh rule below.  The store is rebuilt at every
save of the referrer document, from a document save hook
(`signalStartSaveDocument`, before the collect pass) installed by the
Part module: one entry per distinct target, a healthy reference
refreshed from the live foreign shape, a missing one keeping the child it
has, everything else dropped.  Bare geometry, no element maps.  The
serving side widens `searchElementCache` to carry the referring
property: the feature answers from its own generations first, then asks
the referrer document's store, searching its live shape for the stored
sub-shape as the local path does -- which is also where the mapped-name
caller change of 7.11 lands.  Before building it, two things to verify:
that `App::Document` saves and restores dynamic properties, and whether a
`PropertyPartShape` with no owning object gets blob storage (`makeBlob`
finds the document through the owner object); the fallback is a hidden
document object.  And the ordering trap: the compound must be restored
before the XLinks re-resolve on load.

Gate for what was built, on `build/win-relwithdebinfo-801`:

| check | result |
| --- | --- |
| `FreeCADCmd -t ShapeStorage` | 42 of 42 (the repoint case added; gates 5 and 6 assert release before any save) |
| sec 2.3 matrix | identical to 7.11 |
| C++ (`ctest -j 6`) | 474 entries passed, 0 failed, 1 disabled |
| Python (`FreeCADCmd -t 0`) | 1325 ran; the same 6 failures + 8 errors as 7.11, all this box's environment |

*Next was the foreign store; built in 7.14, with V3 folded in.*

### 7.14 The foreign store and the reload request, built (2026-09-06)

The store of 7.13 exists, and the reload flow of 7.5 (V3) with it, since
the store is useless without a request to serve.

**The two checks, answered.**  `App::Document` saves and restores its
own dynamic properties through the same `PropertyContainer::Save` /
`Restore` an object uses (`Document.cpp:1142`, `:1231`), and the dynamic
restore resolves the type name with module loading
(`Base::Type::getTypeIfDerivedFrom(..., true)`, `DynamicProperty.cpp:184`),
so a `Part::PropertyPartShape` on the document loads Part before any of
the document's objects do.  A document-owned shape property does get
blob storage: `blobManager()` reaches the manager through
`getOwnerDocument()`, which a document answers with itself, and
`serveFromBlob` already tolerated a missing owner.  The one gap was
`ensureRestored()`, which cleared the pending flag and gave up when the
container was not an object; the blob branch now runs without one.  No
hidden document object was needed.  The ordering trap is answered by the
restore sequence itself: document properties come first in the XML pass,
the blobs are dispatched before `afterRestore`, and the links re-resolve
in `afterRestore` -- the compound is a parked blob parsed on the first
request, which is exactly when the XLink asks.

**`Part::ForeignBaseShapes`** (`ForeignBaseShapes.h/.cpp`): two dynamic
properties of the referrer document, `_ForeignBaseShapes` (a compound of
bare sub-shapes) and `_ForeignBaseShapeRefs` (reference name -> child
index, 1-based), group `BaseShape`, hidden and read-only.  Rebuilt from
`App::Application::signalStartSaveDocument`, connected in the Part module
init, before the collect pass: the references are enumerated from the
element-reference registry by a new `PropertyLinkBase::
getExternalElementReferences(doc)` (every feature in another document
that a property owned by `doc` references, with those properties), each
sub-name resolved with `resolveElement`, and the key formed as ruled:
the XLink's persisted path (`getFilePath(getPathResolveMode())`, which is
what its `Save` writes) + `#` + object + sub-name path ending in the
indexed element (`part.FCStd#Cut.Face3`); a reference through a local
object -- a `Part::Plane` attached through an `App::Link` -- is
`Link.Face3`.  A healthy reference is refreshed from the live foreign
shape (the feature's own shape space, which is what the search runs
against); a missing one keeps the child it has; everything else is
dropped; an empty store removes both properties, so a document with no
foreign reference is byte-for-byte what it was.  The rebuild is skipped
when nothing changed (same keys, same children by `IsSame`), so a re-save
keeps its file.  The properties are added and removed through
`PropertyContainer`'s own interface rather than `Document`'s override:
the store is a save artifact and must not open or enter an undo
transaction (the Gui save command runs under a pending auto-transaction
name).

**The widened search.**  `GeoFeature::searchElementCache` takes the
referring property, its linked object and the sub-name path ending in the
old indexed element; `_updateElementReference` passes them.  `Part::
Feature` answers from its own generations first, then, for a referrer in
another document, from that document's store, searching the live shape
for the stored sub-shape exactly as the local path does (one helper,
`searchLiveShape`, serves both).

**The reload request (V3).**  A reference is asked about when it is
*restored* and missing.  Three changes in `PropertyLinks.cpp`, all in
the restore path: `_updateElementReference` treats a call with no
feature as a restore when the property carries `LinkRestoring`
(`PropertyXLink::restoreLink`, the linked document arriving later) or
its document is `Restoring` (`onContainerRestored`, the same document's
`afterRestore`), and then the old element is the indexed name the file
carried -- which the missing marker preserves, so `?Face3` asks for
`Face3`; the "unchanged" early return (`shadow == elementName`) is
skipped for a missing reference being restored, because the XML pass
resolves the same reference once before the shapes have arrived and
would otherwise have used up the request; and `_registerElementReference`
sends a sub-name carrying the marker through `_updateElementReference`
even when its shadow was restored, which is the branch that registered
without looking.  The request runs with `notify` off, as every
restore-time resolve does: a repaired reference changes the sub-name in
memory and the document comes up not touched; the next save writes the
new name and, on the feature side, lets the generation go.  In-session
behaviour is unchanged: a reference marked missing is not searched again
at the next change, so a face that comes back is recovered on the next
reload, not before (`testAReferenceComesBackOnReload`).

**Which references the reload request reaches, found while testing.**
A link property persists its sub-name in the old, indexed form and says
which kind of reference it is by the attribute next to it
(`PropertyXLink::Save`, `PropertyLinkSub::Save` likewise): a reference
held by *mapped name* -- what a selection produces -- is written
`sub="Face1" shadowed=";Face1;:H223,F.Face1"` and comes back with the
mapped name as its sub-name, so the reload resolves it by mapped name;
one held by *index* -- what a script gives -- is written
`sub="Face1" shadow="..."`, and the `shadow=` mapped name is only the
in-session tracker: on reload the reference is resolved by index, by
design (`restoreLink` re-resolves the sub-names and keeps a restored
shadow only for a missing one).  So the headline case is served: the
part edited while the assembly is closed changes every mapped name, the
assembly's mapped references come back missing on reopening and ask the
store, the unchanged side face is repaired and the moved top face is not
(`testAnEditWhileClosedIsRecoveredOnOpen`,
`testAFaceMovedWhileClosedStaysMissing`, a FeaturePython with a plain
`App::PropertyXLinkSubList`).  An indexed reference is never in that
position; what the store does for it is the retry of a reference that
broke with both documents open and was saved missing
(`testAReferenceComesBackAcrossDocuments`).  A first reading of this
section had the mapped name discarded on reload, which was the
SubShapeBinder rewriting its support to indexed names (`setLinks`), not
the link; the first test referrers were binders, and the two tests above
are the correction.

**One gap, not closed.**  A reference to a *local* object that leads to
the foreign feature -- an `App::Link` in the assembly, referenced as
`Link.Face3` -- is not verified on reload at all: the same-document
restore registers a restored shadow without resolving it
(`_registerElementReference`, the branch that only registers), and the
reference is next checked when its target changes, which a part edited
while the assembly was closed never triggers.  It reads as saved,
healthy or not, whatever the part did.  For a reference between objects
of one document the assumption holds -- they were saved together -- and
that is what the branch was written for; through a link it does not.
The fix is to resolve the restored shadow during the document's restore
and let the request run (the store already has `Link.Face3`), but that
touches the map-version upgrade path, where `reverse` re-resolves from
the same shadow, so it was not done here.  Section 7.16 probes that
path and closes the gap; the two tests assert `Link` like `Cut` since.

**Three smaller things the tests settled.**  A `Part::Plane` cannot be
the referrer in a reload test: its attach extension re-sets the support
from the attacher's stored sub-names by position at every recompute
(7.12), which repairs a missing reference in-session whenever the index
exists again -- `testAReferenceComesBackOnReload` uses a SubShapeBinder.
A binder's reference, given by mapped name, is persisted as
`sub="Face3" shadow=";Face6;:M;CUT;:H9c0:7,F.Face3"`, and once missing
its sub-name reads `;Face6;...,F.?Face3`: the indexed name is after the
last dot, which is where the store reads it.  And two dangling references
to a shape returned by value, `ShapeVersion::geometry()` (V2's, caught
by MSVC's C4172) and the store's `children()`, are fixed; the second one
was why the store came back empty after a break.

**Not built, deliberately.**  A restore-time repair does not touch the
referrer: the geometry search found the *same* geometry, so the saved
result is still right.  The Sketcher's prefixed properties are served
from the store like any element (the prefix is on the indexed name), but
no test covers them until V5.

Gates, on `build/win-relwithdebinfo-801`:

| check | result |
| --- | --- |
| `FreeCADCmd -t ShapeStorage` | 51 of 51: the 7 `BaseShapeCases` plus 2 reload cases, and 7 `ForeignBaseShapeCases` |
| sec 2.3 matrix | identical to 7.11: `Face1`, `Face6` -> area 500.00, `Face3` -> `?Face3` |
| C++ (`ctest -j 6`) | 473 of 473 passed, 1 disabled |
| Python (`FreeCADCmd -t 0`) | 1332 ran; the same 6 failures + 8 errors as 7.13, all this box's environment (no `yaml`/`ply`, CRLF material fixtures, two over-long blob paths, one FEM file lock), not one new |

Next: section 7.15.

### 7.15 Schedule for the next session (written 2026-09-06)

In value order, each its own commit, each gated as before (ShapeStorage,
the sec 2.3 matrix, ctest, the Python suite against the known set):

1. **The through-link reload gap of 7.14, characterized before it is
   touched.**  An assembly places parts with `App::Link`s, so `Link.Face3`
   is the common shape of a reference, and today it is not verified on
   reload at all.  First a probe, not a fix: a document whose element map
   version is behind (force it by editing `ElementMap=` in the XML of a
   saved file, or by a parameter that changes the version string) opened
   with a plane and a binder referencing the cut, to see what the
   `reverse` regeneration does with a restored shadow -- whether it
   resolves from `shadow.first`, what it does when `shadow.second` is
   empty, and whether it recovers by geometry at all.  The answer decides
   the shape of the fix: resolve restored shadows during the document's
   restore (`_registerElementReference`, the branch that only registers)
   and let the reload request run, with the `reverse` path stripping the
   missing marker the way the restore path now does, or a narrower rule
   keyed on the target being reached through a link to another document.
   Gate: `testAnEditWhileClosedIsRecoveredOnOpen` and
   `testAFaceMovedWhileClosedStaysMissing` assert `Link` like `Cut`, and
   an upgrade case shows no reference newly missing.

2. **V5, the transaction gates.**  Widen the `onBeforeChange` gathering so
   a break inside undo/redo is seeded; the persistence decision stays at
   save time (D3).  Gate: `testUndoTakesTheGenerationWithIt` unchanged,
   plus a case that breaks a reference inside `openTransaction` /
   `commitTransaction` without a recompute outside it and finds the
   generation retained.

3. **The reload-repair decision, put to the user, not built.**  A repair
   made at reload leaves the referrer untouched (7.14, "Not built,
   deliberately"): the geometry search found the same geometry, so the
   saved result stands, and the document opens clean.  The alternative is
   `addRecomputeObject` on the referrer, which is what a map-version
   mismatch does.  One question, with the trade-off stated.

4. **V5, the Sketcher prefix**, only if the upstream Sketcher merge has
   happened; otherwise it stays parked.  A generation of a prefixed
   property is materialized under a name that carries the property
   (`_BaseShape<N>` with a `_BaseShapeProps` entry, or a prefix in the
   manifest value), and `adoptShapeVersions` reads it back.

Before any of it, a ten-minute hygiene pass: grep the Part module for
`getShape().getShape()` and `getShape().` bound to a reference -- the
two dangling references of 7.14 were the same mistake twice, and
`PropertyPartShape::getShape()` returning by value invites a third.

### 7.16 The through-link reference, verified on reload (2026-09-06)

Item 1 of 7.15: probed first, then built.  The hygiene pass before it
found two more references bound to `Shape.getShape().getShape()`, both
outside Part -- `Fem/Gui/TaskFemConstraintFluidBoundary.cpp` and
`TechDraw/App/FeatureProjection.cpp` -- and both copy now; the remaining
uses in `PrimitiveFeature.cpp`, `ViewProviderExt.cpp` and
`DlgProjectionOnSurface.cpp` were already by value.

**The probe.**  Model A's part; an assembly with an `App::Link` to the
Cut and a FeaturePython holding an `App::PropertyXLinkSubList` to `Cut`
and to `Link`, `Face1` and `Face3` each, by mapped name; the part's file
aged one map version by rewriting `ElementMap="1.15.80001.4"` to `1.14.`
in its `Document.xml`; then opened through the assembly, recomputed,
saved, with the part unchanged (A), re-saved (A2), edited at the same
height (B) or 5 mm taller (C) while the assembly was closed.  Three
findings:

1. **The upgrade is requested only for a part opened whole.**  A part
   opened through the assembly is a *partial* document whenever the
   assembly does not reach every object in it, which the edit produces at
   once (the old base box stays in the file, unreferenced):
   `PropertyPartShape::Restore` skips the version check for a partial
   document, and `recompute()` on it refuses ("Please reload partial
   document").  The Cut still recomputes -- with the *assembly's*
   recompute, which crosses into the documents it depends on -- and that
   is where `reverse` runs: after the assembly's references are fully
   restored, in-session, over every reference registered under the Cut,
   the through-link ones included (A: all four re-resolved, unchanged).
2. **What `reverse` does with a restored shadow.**  It resolves
   `shadow.first`, the mapped name; when that fails it takes the indexed
   name from `shadow.second` and asks `searchElementCache`, which answers
   from the feature's own newest generation first -- the shape the Cut
   had *before this recompute*, gathered by `onBeforeChange` -- and from
   the store only after.  Right for a same-session break; wrong for a
   through-link reference never verified on reload, which it confirms
   against the wrong baseline: in C the direct reference came back
   `?Face3` at open (the store said so), while the through-link one was
   "repaired" by the upgrade to whatever `Face3` is now (`auto change
   element reference asm#Link ... -> ;Face6;:M;CUT;:H690:7,F.Face3`), the
   face 5 mm away.  An empty `shadow.second` gives no old element and no
   search; nothing to do there.
3. **A missing reference is blanked by the regeneration.**  After the
   reload request leaves a reference `?Face3`, the same-document
   re-registration resolves it once more (`_registerElementReference`
   sends every marker-carrying sub through `_updateElementReference`),
   and `?Face3` looked up as a name is an unknown *mapped* name with no
   indexed one: `_getElementName` answers `("?Face3", "")`, which is not
   "missing", so the shadow becomes `("?Face3", "")`, saved as
   `<Sub value="?Face3" shadow="?Face3"/>`.  The next `reverse` writes
   `shadow.second` -- empty -- into the sub-name, and the reference
   silently becomes the whole Cut (a `Part.Compound` where a face was).
   In-session (D: the version property forced stale and the Cut
   touched) the shadow still holds the old mapped name and the reference
   stays `?Face3`; the blank needs the reload first.

**Built.**  Two changes in `PropertyLinks.cpp`:

- `_registerElementReference`: a restored shadow is taken as it is only
  when the geometry it names lives in the referrer's document.  When the
  resolved feature is in another document -- reached through a link --
  and the document is restoring (`Document::Restoring`, or the property's
  `LinkRestoring`), the reference goes through `_updateElementReference`
  instead, which is the reload request of 7.14: a healthy one is
  unchanged (`shadow == elementName`, one resolve), a missing one asks the
  store under `Link.Face3`.  The narrower rule of 7.15 was chosen over
  resolving every restored shadow: same-document references were saved
  together, and re-resolving all of them on every open is cost with no
  case behind it.
- `_updateElementReference`: an element that is itself the missing marker
  is missing by construction -- `elementName` set to the marker name,
  `missing` true -- before the search and the write-back, so the
  regeneration keeps `?Face3`, and the reload request may still recover
  it.

Tests (`ForeignBaseShapeCases`): the two edit-while-closed cases assert
`Link` like `Cut`, and the recovered one checks that the saved
`shadowed=` names are the Cut's current ones for both;
`testAnUpgradeKeepsEveryReference` ages the part file (`ageElementMap`),
reopens, recomputes through the assembly and finds nothing newly
missing and the file on the current names;
`testAMissingReferenceSurvivesTheUpgrade` does the same over the moved
face and finds `?Face3` on both, in memory and in the file.

Gates, on `build/win-relwithdebinfo-801`:

| check | result |
| --- | --- |
| `FreeCADCmd -t ShapeStorage` | 53 of 53: the 7 `BaseShapeCases` plus 2 reload cases, and 9 `ForeignBaseShapeCases` (the 2 upgrade cases added) |
| sec 2.3 matrix | identical to 7.11: `Face1`, `Face6` -> area 500.00, `Face3` -> `?Face3` |
| C++ (`ctest -j 6`) | 473 of 473 passed, 1 disabled |
| Python (`FreeCADCmd -t 0`) | 1336 ran; the same 6 failures + 8 errors as 7.14, all this box's environment (no `yaml`/`ply`, CRLF material fixtures, two over-long blob paths, one FEM file lock, the tetra10 yml import), not one new |

Next: section 7.17.

### 7.17 The transaction gates, measured: nothing to widen (2026-09-06)

Item 2 of 7.15 asked for the `onBeforeChange` gathering to be widened so
a break inside undo/redo is seeded.  Probed first (`probe_v5.py`, model
A with a plane on `Face3`, `UndoMode = 1`, the generations and the
references printed after every step), and the premise did not hold.

**What the gate excludes.**  `Document::isPerformingTransaction()` is
`undoing || rollback || Transaction::isApplying()`: true while a
transaction is *applied* -- undo, redo, abort -- and false while one is
merely open.  A break inside `openTransaction` / `commitTransaction` is
therefore gathered like any other, before the commit
(`testABreakInsideAnOpenTransactionIsSeeded`: `_BaseShape1` and the
manifest are there with the transaction still open; the abort, which
restores the recorded properties, takes them away and the plane reads
`Face3` again).  D3's stated cost -- "a break that happens entirely
inside a transaction is never seeded" -- described the open transaction,
and that was never the case.

**What the apply of a transaction does.**  The generation is a dynamic
property and the manifest a property; both are recorded in the same
transaction as the reference that broke and the shape that changed, so
every state an undo or redo restores is self-consistent (`U1`: break,
repair, undo the repair -> `?Face3` and `_BaseShape1` are back together;
redo -> both gone; undo twice, redo -> the break again, with its
generation; `testUndoOfARepairBringsTheGenerationBack`).  A shape change
applied by a transaction replaces the outgoing shape with a *recorded*
one; there is no new generation to keep, and none is lost.  Widening the
gate would gather a generation the reconcile releases at once (no local
referrer is missing against it), for nothing.

**The one thing undo does leave behind, and why the gate is not the
answer.**  `GeoFeature::onChanged` skips `updateElementReference()`
while a transaction is applied, so a reference in *another* document --
not part of the record -- is not re-resolved by the part's undo.  `U3`:
the assembly's `?Face3` stays `?Face3` after the part's undo brings the
face back (and stays so until the next reload, by the in-session rule of
7.14).  `U4`, the other direction: an assembly reference made while the
part was 25 tall reads healthy after the part's undo to 20, resolving to
the face at z=20 by index, until the part's next change re-resolves it
by geometry from the gathered generation (the z=20 shape, not its
z=25 baseline) and marks it `?Face3` -- conservative, and the assembly's
own store holds the z=25 face for the reload.  Neither is a gathering
problem: the feature retains for local referrers only (7.13), and a
foreign referrer's evidence is its own store.  If anything is to be done
it is a re-resolve of foreign referrers after a transaction is applied,
which is a question for the user, put with item 3 of 7.15 (below).

**Not built.**  No gate changes; two tests record the answer.  D3 stands
with its cost corrected: the persistence decision at save time loses
nothing to transactions.

Gates, on `build/win-relwithdebinfo-801` (tests only, no C++ change
since 7.16):

| check | result |
| --- | --- |
| `FreeCADCmd -t ShapeStorage` | 55 of 55 (the 2 transaction cases added) |
| sec 2.3 matrix, C++, Python suite | as 7.16, the binaries unchanged |

**For the user, before the next step (items 3 of 7.15 and the above):**

1. A repair made at reload leaves the referrer untouched: the geometry
   search found the *same* geometry, so the saved result stands and the
   document opens clean.  The alternative is `addRecomputeObject` on the
   referrer, as a map-version mismatch does -- the document opens
   touched and asks for a recompute, which redoes work whose input has
   not changed but makes the repair visible.  Which?
2. Should references from other documents be re-resolved after a
   transaction is applied (undo/redo/abort) in the referenced document,
   so that an assembly's reference follows the part's undo in-session
   rather than at the next change or reload?  It costs a resolve pass
   over foreign referrers per applied transaction, and it changes
   documents that the undo was not opened in.

Next: section 7.18, the answers.

### 7.18 The two questions, answered (2026-09-06)

**D6, the reload repair stays silent -- when the search found the same
shape.**  The principle, as the user put it: if the repair or resolve
finds the same shape by geometry comparison it can be silent; otherwise
the reference must be marked missing to get the user's attention.  The
silent fix may be extended later, not now.  That is what 7.14 and 7.16
built: a reference repaired from a retained generation or the foreign
store keeps the saved result, the document opens clean, and a face that
moved reads `?Face3`.  No recompute is requested for a repair.

**D7, no re-resolve pass after a transaction is applied.**  By the
recompute logic, the referenced document's recompute marks the referrer
for recompute, and the resolve happens when the referrer itself
recomputes -- not by a pass of its own after an undo.  Measured (`U4`
extended): after the part's undo the Cut is touched; the assembly's
recompute recomputes it as a dependency, the Cut's shape change
re-resolves the assembly's reference, and the reference made at z=25
follows by index and geometry to the top face at z=20 (`auto change
element reference ... :Hfc3:7,F.Face3 -> ... :Hfbf:7,F.Face3`).  Silent,
by the recompute flow, and against the only baseline the feature holds
in-session -- the shape it had before that recompute, since a foreign
referrer's own evidence is written at save (7.13) and not kept live.
Accepted as the consequence of "local referrers only"; keeping a foreign
referrer's baseline live in-session is the extension D6 leaves for
later.

With these, items 1 to 3 of 7.15 are closed and item 4 waits for the
upstream Sketcher merge.  The pre-task is complete for the `Shape`
property; the main task, section 3 (TechDraw adopts element names), is
next.


## 8. The main task, built

Section 3 is the plan; this section is the state, one subsection per
step of the build order in 3.6.  Each step is gated the way section 7's
were: the TechDraw suites, `ctest`, the Python suite against the known
Windows set, and evidence that the step did what it claims.

### 8.0 The gate harness, first (2026-09-06)

TechDraw's meaningful tests do not run under `FreeCADCmd`.
`TestTechDrawApp` is six cases -- hatch, annotation, balloon, image,
symbol, projection group -- and the four suites that actually project
geometry (`DrawViewPartTest`, `DrawViewSectionTest`, `DrawViewDetailTest`,
`DrawViewDimensionTest`) are imported by `TestTechDrawGui` instead,
because each waits for the HLR threads on a `QEventLoop` driven by a
`QTimer`.  Console mode dispatches neither, so `FreeCADCmd -t
TestTechDrawGui` **hangs** rather than failing -- it was killed at two
minutes with no output.

They need only a running application, not a display of its own, so on
this box they run the way `docs/Testing.md` describes for the two
registered GUI tests: a startup script under

    run_cdb.ps1 -UserHome <isolated dir> -StartupScript <script>

that loads both module lists into one `unittest` suite from a
`QTimer.singleShot(0, ...)`, writes PASS/FAIL and `DONE` to a result
file, and quits.  **11 tests, 28 s**, and that is the number every
subsection below quotes.  The script is a scratchpad file, not
committed, in keeping with the probe convention.

### 8.1 T0, built: the element map reaches the view (2026-09-06)

Probed first, and the probe is why the step was an afternoon rather than
a week.  Every form a TechDraw source takes already carries an element
map out of `Part::Feature::getTopoShape`, and every assembly step the
extractor performs has a name-propagating equivalent that keeps it:

| source | `ElementMapSize` | a face name |
| --- | --- | --- |
| `Part::Cut` feature | 32 | `Face1;:H961,F` |
| the same through an `App::Link` | 32 | `Face1;:H961,F;:H963,F` |
| an `App::Part` holding a box | 26 | `Face1;:H964,F` |
| `makECompound` of two of them | 64 | both, unchanged |
| `makEFuse` of two of them | 64 | `...;:M;CUT;...;:H965,F` |
| `transformed()` (move, scale, mirror), `Placement =` | unchanged | unchanged |

So nothing had to be invented: `ShapeExtractor` was throwing the map
away by asking for `TopoDS_Shape` and rebuilding compounds with a bare
`BRep_Builder`.

**What changed.**  `ShapeExtractor::getShapes`, `getShapes2d`,
`getShapesFromObject`, `getShapesFused`, `getLocatedShape` and
`stripInfiniteShapes` return `Part::TopoShape` (or a vector of them);
the compounds are built with `makECompound` and the fuse with
`makEFuse` in place of a pairwise `BRepAlgoAPI_Fuse` loop;
`getLocatedShape` asks `getTopoShape` instead of `getShape`.
`DrawViewPart::getSourceShape` returns `Part::TopoShape` with it.  The
callers that still want the raw shape -- `DrawBrokenView` (ten
`getLocatedShape` sites and the source), `DrawViewMulti`,
`DrawViewSection::getShapeToCut`, `ShadedUnderlay`,
`DimensionValidators`, and `DrawViewPart::execute` itself -- take
`.getShape()`.  Nothing downstream of `partExec` changed: the names
reach the projection input and stop there, which is T2's job.

**The one trap, and it is a real one.**  `makECompound`'s `force`
parameter reads the opposite way from its doc comment.  The header says
"if true and there is only one input shape, then return that shape
instead"; the code (`TopoShapeEx.cpp:1586`) returns the lone shape when
force is **false**, and the default `true` always wraps.  Passing
`false` on the strength of the comment made a single-source view return
a `Solid` where every caller since 2019 has had a `Compound` --
`DrawViewMulti` casts the result with `TopoDS::Compound` -- and the
TechDraw suites still passed, because none of them has a
`DrawViewMulti`.  The probe caught it: the evidence script prints
`ShapeType`, and it read `Solid`.  Print the type in any probe that
replaces a compound builder.

**Evidence** (`view.getSourceShape()`, the accessor added below):

    one source           Compound  elementMapSize=32
        Face1 -> Face1;:H8d8,F;:H8da,F;:H:8,F
        Face3 -> Face6;:M;CUT;:H8d8:7,F;:H8da,F;:H:8,F
    two sources          Compound  elementMapSize=58
    two, fuse=True       Compound  elementMapSize=90
        Face3 -> ...;:M;CUT;...;:M;FUS;:H8da:7,F
    through an App::Link Compound  elementMapSize=32
        Face1 -> Face1;:H8d8,F;:H8da,F;:H8df,F;:H:8,F

and the geometry is untouched: the two-source compound is V=18 E=27
F=13, two solids, 1874.3363 mm3, and the fused one V=26 E=45 F=19, one
solid, 1594.0167 mm3, before and after.

**A Python accessor came with it**, because there was no way to see a
view's source shape from a test: `DrawViewPart.getSourceShape(fuse=False)`
returns the compound as a `Part.Shape`, element map and all.  It is what
the evidence above is read from, and T2's tests will read the projected
names against it.

The composite path was probed too, since `stripInfiniteShapes` no longer
walks the compound with a `TopoDS_Iterator` but with
`getSubTopoShapes()`: an `App::Part` holding a box and a
`PartDesign::Plane` (an infinite `Face`) projects a compound of the box
alone -- 6 faces, bound box `(0,0,0,10,10,10)`, `Face1 ->
Face1;:Hbc8,F;:Hbc6,F;:H:8,F` -- so the infinite child is dropped and
the finite one keeps its name.

**Gates.**  TechDraw 11 of 11 (28.0 s); `ctest` 477 of 477 (26 s, `-j 6`);
the Python suite unchanged against this box's known set -- 1338 tests,
6 failures and 8 errors, the same ones, every one environment rather
than geometry (no `yaml` for CAM, CRLF fixtures in the MaterialX graph
tests, two over-long temp paths, and four FEM cases).

**Not done here, deliberately**: `ShapeUtils::rotateShape` /
`mirrorShape` / `scaleShape` and `DrawUtil::shapeVectorToCompound` still
take and return `TopoDS_Shape`.  They sit between the source shape and
HLR, so T2 has to make them name-preserving (`makETransform` /
`makEGTransform` do it, as the probe table shows); doing it now would
have been churn with no consumer.

### 8.2 T1, built: the section cut is named from its tool (2026-09-06)

Section 3.4 called the anonymous cut the load-bearing weakness of the
section views, and 3.6 called T1 the highest-value change for section
stability.  It is built for `DrawViewSection`; the complex section's
tool is a different construction and is deliberately left for T1b
(below).

**Probed first, four questions, and two of the answers changed the
design.**

1. *Does an unnamed tool really leave the section face anonymous?*  No
   -- worse than anonymous, it leaves it named after the intersection
   that made it.  Cutting a named box with an unnamed prism gives the
   cut face

       Face1;:G;CUT;:H9d7:7,E;:L(Face2;:G;CUT;:H9d7:7,E|Face3;...|Face4;...);CUT;:H9d7:4d,F

   a combo name built from the edges the cut created.  Every edge in
   that list is a place where the tool met the model, so any edit that
   changes which faces the plane crosses rewrites the name.  That is
   the instability, spelled out.

2. *Can the tool's faces be named by hand?*  Yes:
   `setElementName("Face5", "SectionTool5")` on a `TopoShape` with no
   hasher stores exactly that name, and the cut then reports the
   section face as `SectionTool5;:M;CUT` -- one step, no combo.

3. *Is the name stable when `SectionOrigin` moves?*  Yes.  The same
   tool named and moved to z=10, 12 and 8 gives `SectionTool5;:M;CUT`
   every time.

4. *What does the shape handed to the cut carry?*  **A document's
   `App::StringHasher`.**  `cut.Shape.Hasher` and
   `Part.getShape(cut).Hasher` are the document's own hasher object,
   and `App/StringHasher.{h,cpp}` contain no mutex, no lock, no atomic
   -- only `Base::Handled`'s refcount is atomic.  The section cut runs
   in a `QtConcurrent` worker, and mapping element names *writes* to
   the hasher (`mapSubElement` adopts the source's hasher outright:
   `Hasher = other.Hasher`).  So the obvious implementation -- call
   `makEBoolean` in the worker -- would have had a background thread
   hashing into a document-owned table while the main thread finished
   the recompute.

**The shape of the fix that question forced.**  The geometry stays in
the worker and the naming happens on the main thread:

- `doSectionCut` runs the same `BRepAlgoAPI_Cut` calls as before -- and
  keeps them, in a `CutHistory` (the source solid and the maker per
  piece, plus the trim pass).  Nothing about the boolean changed, so
  the progress indicator and the cancel path are exactly as they were.
  That is also why the cut is not simply `makEBoolean`: that API takes
  no `Message_ProgressRange`, and a section cut of a real model is
  where a progress bar earns its keep.
- `onSectionCutFinished`, which already runs on the main thread when
  the future lands, calls `nameCutPieces`: `makEShape(*maker, {source,
  tool}, OpCodes::Cut)` per piece, `makECompound` over them, then the
  trim pass the same way.  `makEShape` on a maker is what `makEBoolean`
  does internally after `Build()`, so the names are identical to the
  ones a plain `makEBoolean` would have produced.
- Any failure -- no history, an aborted cut, a derived class that cuts
  its own way -- falls back to the unnamed compound.  The names are an
  addition; nothing depends on them yet.

**The tool's names are decided by geometry, not by face order.**
`makeCuttingTool` builds the same prism it always did, then
`nameToolFaces` walks its planar faces and names each one from the
section plane's own coordinate system: the face lying in the plane is
**`SectionPlane`**, the parallel one behind it `SectionBack`, and the
four sides `SectionSideXMin/XMax/YMin/YMax` by the axis they face
along.  Nothing in that depends on `shapeSize`, on where the plane
sits, or on the order `BRepPrimAPI_MakePrism` happened to build the
faces in -- which is what makes the resulting name survive a move.

**Evidence**, from a section through a box with a hole, driven in a
running application because the cut is asynchronous:

    tool faces      : 6, elementMapSize=6
    tool names      : [SectionBack, SectionPlane, SectionSideXMax,
                       SectionSideXMin, SectionSideYMax, SectionSideYMin]
    cut pieces      : Compound, faces=7, elementMapSize=32
    section face at z=10: SectionPlane;:M;CUT;:H,F
    SectionOrigin 10 -> 14, same name
    the hole's radius 3 -> 5, same name

Three PASS lines: the name survives moving `SectionOrigin`, survives a
model edit, and is derived from the tool's `SectionPlane` face.

**A Python type for the section came with it.**  `DrawViewSection` had
none -- it answered to `DrawViewPartPy` -- so there was no way to look
at a cut result at all.  `DrawViewSectionPy` now carries
`getCutPieces()` (the cut result, named, in the global frame) and
`getCuttingTool()` (the tool of the last cut, with its six names).  T3
has a home for the section-face API when it needs one.

**Types that moved with it**, all mechanical: `getShapeToCut`,
`makeCuttingTool`, `getShapeToPrepare`, `m_cutPieces`, `m_cutShapeRaw`,
`m_saveShape` (on `DrawViewPart`) and the `sectionExec`/`makeSectionCut`/
`prepareShape` signatures take or return `Part::TopoShape`.  The copy
of the cut input is `makECopy` rather than `BRepBuilderAPI_Copy`, which
is the same copy plus the element map.

**Gates.**  TechDraw 11 of 11 (27.5 s); `ctest` 477 of 477 (26 s);
the Python suite unchanged against this box's known set.

**Left for T1b**: `DrawComplexSection::makeCuttingTool` returns its
tool unnamed.  Its tool is extruded from the user's own profile object,
so the right names for its faces are not a fixed scheme like the six
above -- they are the profile's own element names, carried through the
extrusion.  That is a better identity than this one (it says *which
segment of the profile* made the face) and a different piece of work.

**Left for T2**: `prepareShape` still centers, scales and rotates
through `ShapeUtils`, which is `TopoDS_Shape` in and out, so the names
stop at `m_cutPieces` / `m_cutShapeRaw`.  And the 2D section faces are
rebuilt from projected wires in `mapToPage`, so they can never inherit
a name through the shape: they need the name carried beside them, in
the `ref3D` slot `BaseGeom` reserves.  That is what T2 is for.

### 8.3 Schedule for the next session (written 2026-09-06)

The user's instruction, given at the end of the T1 session: **the next
session does T1b and T2.**  In that order, each its own commit, each
gated as 8.0 to 8.2 were -- the TechDraw 11, `ctest`, the Python suite
against this box's known set, and evidence that the step did what it
claims.

**T1b, the complex section's tool.**  `DrawComplexSection::makeCuttingTool`
returns three different shapes depending on the profile: a prism from a
face when the profile wire is closed, a prism from `m_toolFaceShape`
(the wire extruded sideways) when it is open, and a compound of the
solid pieces of that prism when the extrusion produces empty ones.  A
fixed six-name scheme does not fit any of them.  The names should come
from the **profile object's own elements** instead, which is a better
identity than `SectionPlane`: it says which segment of the profile made
the face, so a section whose profile gains a segment keeps the names of
the segments it already had.

The route: fetch the profile with `Part::Feature::getTopoShape` rather
than as a bare `TopoDS_Wire` (`makeProfileWire`), and build the tool
with the name-propagating makers -- `makEFace` for the closed case,
`makEPrism` (`TopoShape.h:1598`, op code `PSM`) for both extrusions.
The compound-of-solids filter is already a loop over the prism's
solids, so it becomes `makECompound` over the kept ones.  Probe first,
as ever: check that a profile sketch's edges arrive named through
`getTopoShape`, and that `makEPrism` puts those names on the side faces
of the extrusion.  Then the same evidence as 8.2 -- move the profile,
change the model, the names hold -- with the section-face name reading
back through `getCuttingTool()` / `getCutPieces()`.

**T2, carry names through HLR.**  The hard one, and the one 3.5 already
did the research for: `HLRBRep_Data::EDataArray()` is indexed like
`EdgeMap()`, so an in-tree reimplementation of
`HLRBRep_HLRToShape::InternalCompound` (about 60 lines) emits the same
edges in the same order **and reports the source edge index with each**.
The polygon path (`HLRBRep_PolyAlgo`, `projectShapeWithPolygonAlgo`,
`GeometryObject.cpp:306`) hands the source shape back directly through
`Hide`/`Show`.  Both entry points are in `GeometryObject.cpp`
(`projectShape` at 143, the poly one at 306), each currently calling the
stock `HLRToShape`/`PolyHLRToShape`.

Two things have to be in place before the traversal is worth writing:

1. The projection input has to still be named when it reaches HLR.
   Today it is not: `DrawViewPart::partExec` takes a `TopoDS_Shape`, and
   `ShapeUtils::rotateShape` / `mirrorShape` / `scaleShape` /
   `centerShapeXY` and `DrawUtil::shapeVectorToCompound` are all
   `TopoDS_Shape` in and out.  They need `Part::TopoShape` overloads
   built on `makETransform` / `makEGTransform`, which the 8.1 probe
   already showed preserve the map (move, scale and mirror all keep
   every name).  That is the first commit of T2 and it is mechanical.
2. `BaseGeom` has to carry the name.  The `int ref3D` slot it reserves
   and never fills (3.3) is where the source element goes -- as a
   mapped name, not an int -- and the projected edge's own name is
   `<sourceElementName>;HLR:<class>:<ordinal>` per 3.6.

Watch for the trap 3.7 records: `DrawViewDimension::execute()` runs
while the document is still restoring, against a view that has not
projected yet, so anything keyed on "have I run once" spends its one
chance there.  And keep T2's own scope honest -- the 2D faces are
rebuilt from projected wires in `mapToPage`, so they take their name
from the wires that bound them rather than from any shape identity,
which is the change that kills the area-sort fragility.

### 8.4 T1b, built: the complex tool is named from its profile (2026-09-06)

8.2 left `DrawComplexSection::makeCuttingTool` returning its tool
unnamed, and 8.3 named the reason: that tool is three different
constructions, and no fixed six-name scheme fits any of them.  It is
built now, from the profile object's own elements.

**Probed first, two questions, and both answers were yes.**

1. *Do the profile's edges arrive named?*  Yes.  A three segment sketch
   used as a profile gives `Part.getShape` a wire whose edges are
   `g1;SKT`, `g2;SKT`, `g3;SKT` -- and `noElementMap=True` does not
   strip them, the same as every other whole-feature shape (8.1).  The
   old code lost them anyway: `makeProfileWire` assigned
   `Part::Feature::getShape` (a `TopoDS_Shape`) into a `Part::TopoShape`,
   which is a conversion, not a copy of the map.
2. *Does `makEPrism` put those names on the extrusion?*  Yes, on both
   sweeps.  Extruding the wire sideways gives a shell whose three faces
   are `#9;:G;XTR;...`, `#a;...`, `#b;...` -- one per profile edge --
   and extruding that shell along the section normal keeps those three
   names on the faces that lie on the cut surface, which are exactly the
   faces the section face is cut from.  The `#` forms are hashed because
   a profile from a document carries that document's hasher; they
   resolve back to the sketch geometry, as the evidence below shows.

**What changed.**  Four new members beside the raw ones they shadow, so
nothing that wanted a bare `TopoDS_Wire` had to change:

- `makeProfileShape` -- the profile as a named `Part::TopoShape`, via
  `getTopoShape` rather than `getShape`.  `makeProfileWire` is now this
  plus `.getShape()`, so there is one implementation of what the profile
  *is*.
- `makeNoseToTailShape` -- `makeNoseToTailWire`'s reordering with the
  names put back.  Nothing is rebuilt but the wire itself, so
  `mapSubElement` finds every edge by identity; a one-edge profile comes
  back untouched.
- `extrudeWireToFace(Part::TopoShape&, ...)` -- the same sweep through
  `makEPrism`, leaving the wire moved the way the raw version leaves it.
- `makeProfileFace` -- `makEFace` for the closed profile, falling back
  to the plain `BRepBuilderAPI_MakeFace` when the name propagating face
  maker will not take the wire.  A profile that used to build a tool
  still builds one.

`m_toolFaceShape` is a `Part::TopoShape` now, so the names survive into
the second extrusion, and the empty-solid filter is a `makECompound`
over the solids it keeps (`force` left at its default, which always
wraps -- the trap 8.1 records).  The aligned worker still receives a
bare `BRepBuilderAPI_Copy` of the tool face: mapping names writes to the
document's `App::StringHasher` and that has no locking (8.2), while
`makeCuttingTool` itself runs on the main thread, from
`DrawViewSection::makeSectionCut` before the future is launched.

The one behaviour change is that the second `BRepPrimAPI_MakePrism` the
old code built and threw away is gone; the prism is computed once.

**Evidence**, driven in a running application, on an Offset complex
section of a 70x40x20 box with a hole, stepped profile
`(-30,0) -> (-5,0) -> (-5,10) -> (30,10)`:

    tool            : Compound, faces=12, elementMapSize=52
    section faces   : #75;:G;XTR;:H6ea:7,F;:H,F;:M;CUT;:H6ea:7,F;:H,F
                      #77;:G;XTR;:H6ea:7,F;:M;CUT;:H6ea:7,F;:H,F
    resolved        : {{g1;SKT};:H6ea,E};:G;XTR;...;:M;CUT;...
                      {{g3;SKT};:H6ea,E};:G;XTR;...;:M;CUT;...

Seven PASS lines: the tool is named; the section face is named; the name
is one step off the tool rather than a combo of the edges the cut
created (`;:L(` -- the instability 8.2 measured); the name resolves
through the document hasher back to a segment of the profile sketch; it
survives `SectionOrigin` moving and the model changing; and when the
profile gains a fourth and fifth segment the two existing section faces
keep their names while the new segment brings a third of its own.

Two things the probe had to get right, both of which made an earlier run
say nothing:

- **A profile segment parallel to the extrude direction makes no tool
  solid**, by design -- that is what the empty-solid filter is for.  A
  three segment stepped profile therefore gives a tool of two solids and
  twelve faces, not three and eighteen, and a "new segment" added along
  the section normal changes the tool not at all.  The added segment has
  to be one that sweeps a volume.
- **The section faces have to be told apart from the model's own faces
  that happen to face the same way.**  Filtering only on the surface
  normal picked up the box's front and back, which arrive named
  `Face2;:H,F` from the source and have nothing to do with the tool.

**Gates.**  TechDraw 11 of 11 (28.5 s); `ctest` 477 of 477 (26 s); the
Python suite 1338 tests, 6 failures and 8 errors, exactly this box's
known environment set.

**Still left for T2**, unchanged from 8.2: `prepareShape` centers,
scales and rotates through `ShapeUtils`, which is `TopoDS_Shape` in and
out, so the names stop at `m_cutPieces` / `m_cutShapeRaw`; and the 2D
section faces are rebuilt from projected wires in `mapToPage`, so they
need the name carried beside them in `BaseGeom`'s `ref3D` slot.

### 8.5 T2, built: names through HLR (2026-09-06)

Two commits, as 8.3 planned them.  The first is the mechanical one that
keeps the names alive as far as the projection; the second is the
traversal that hands them to the projected geometry.

#### 8.5.1 The projection input keeps its names

`1f78bbd54d`.  Every name stopped at the door of the projection:
`DrawViewPart::partExec` took a `TopoDS_Shape`, and the centering,
scaling and rotating between the source and HLR went through
`ShapeUtils`, which is `TopoDS_Shape` in and out.

`ShapeUtils` has `Part::TopoShape` overloads now -- `mirrorShapeVec`,
`mirrorShape`, `invertGeometry`, `scaleShape`, `rotateShape`,
`moveShape`, `centerShapeXY` -- each composing the same `gp_Trsf` as
before and handing it to `makETransform`, which copies the shape when
the transformation scales or mirrors and only moves it when it does not.
The map survives all four, the Y mirror into Qt's coordinate system
included.  The pipeline is typed to match: `partExec`,
`makeGeometryForShape`, `centerScaleRotate`, `buildGeometryObject`,
`DrawViewSection::prepareShape` and its complex-section override, and
`GeometryObject::projectShape` / `projectShapeWithPolygonAlgo`.  The
copy `makeGeometryForShape` takes so it does not transform the source in
place is `makECopy`.  `GeometryObject` keeps what it projected in
`m_projectionShape`, and `DrawViewPart.getProjectionShape()` reads it
back.

Evidence, on a plain view at Scale 2 and Rotation 30, a section and a
complex section: the shape HLR is given has an element map of the same
size as the source (32 of 32), every one of its faces is named, the
section face is still `SectionPlane;:M;CUT`, and the complex section
face still resolves to `{g1;SKT}`.

Two notes for whoever reads the diff.  `DrawUtil::shapeVectorToCompound`,
which 8.3 named, has **no caller anywhere in the tree** and was left
alone.  And the detail, multi and broken views, `DrawProjectSplit` and
`DrawDimHelper` convert to `Part::TopoShape` at the call rather than
through their own pipelines: their inputs are bare `TopoDS_Shape`
already, so a map would stop there in any case -- the explicit
conversion is there to make that visible instead of letting an implicit
one hide it.

#### 8.5.2 Every projected edge knows the element it came from

`1ffb61df60`.  The reimplementation 3.5 argued for, of both entry
points.

`hlrInternalCompound` with `hlrDrawFace` and `hlrDrawEdge` was
`HLRBRep_HLRToShape::InternalCompound` and its two helpers, minus the
branches TechDraw never reaches (there is no shape filter and no `In3d`
output), emitting the same edges in the same order and reporting the
`ie` of each.  **Since 2026-09-17 that traversal lives in
`Part::HLRProjector`** (`src/Mod/Part/App/HLRProjector.{h,cpp}`,
written for the Sketcher's external faces, `docs/SketcherPort.md`) and
`projectShape` runs the class: `edges()` per type and visibility hands
out the compounds, `info()` the source of each edge.  Two things
changed on the way.  The source is now the ORIGINAL input element:
the old copy keyed it on the algorithm's own edge map, where an edge
the outliner split (a cylinder rim a silhouette vertex lands on) is a
new edge, so those went unnamed -- the projector reads the outliner's
data structure back to the input edge, and the rims of a cylinder now
carry names like everything else.  And a silhouette or iso line
reports the face it lies on, and is named from it: `m_edgeSource`
carries `-Face<n>` for those (the polygon path binds its face the same
way), `extractGeometry` splits that into `BaseGeom::ref3D` /
`ref3DFace`, and `nameEdgeGeometry` reads the face's mapped name, so a
cylinder's silhouettes are `Face1;...;HLR:VO:0` and `:1` and the
vertices and faces built on them get names too.  The face index is
saved as a `face` attribute on the existing `Ref3D` element, so a file
from before restores unchanged (no face, unnamed until re-projected).
The projector is handed the shape without its element map, so the
worker never touches the hasher (8.2 still holds).  `polySegments` and `polyInternalCompound` are the same
for `HLRBRep_PolyHLRToShape`: its `Update` pass keeps the shape each
segment came from, which `HLRBRep_PolyAlgo::Hide` fills in, and
`InternalCompound` then filters that list by type and visibility.  The
polygon algorithm's type codes are its own and differ from the exact
one's -- 1 outline, 2 smooth, 3 seam, 4 hard, against 1 iso, 2 outline,
3 smooth, 4 seam, 5 hard -- which is the one thing to get wrong here.

A side effect worth having: the old code called each accessor twice,
once to test for null and once to keep the result, so it ran every one
of the ten traversals twice.  Each runs once now.

**The mirror is the join that had to be got right.**  Between the
traversal and the geometry sits `invertGeometry`, whose transformation
is negative and therefore copies every edge.  `invertAndTrack` composes
exactly the transformation `mirrorShape` composes -- so the geometry is
unchanged -- and carries the association across with
`BRepBuilderAPI_Transform::ModifiedShape`, rather than assuming the copy
preserves the order of a compound's children.

`BaseGeom` carries the result.  `ref3D`, the slot 3.3 found reserved and
never filled, is the index of the source element in the projection
shape's own `Edge<n>` numbering, and the projected edge gets a name of
its own,

    <sourceElementName>;HLR:<class>:<ordinal>

with `class` = visibility (`V`/`H`) plus edge class (`H` hard, `O`
outline, `S` smooth, `E` seam, `I` iso) and `ordinal` telling apart the
fragments one source edge is broken into by hiding.
`DrawViewPart.getEdgeNames()` reads all three back.

**The thread split is T1's.**  The projection runs in a worker and
records nothing but indices and shapes; `DrawViewPart::onHlrFinished`
turns them into names on the main thread, because reading an element map
adopts the document's `App::StringHasher` and that has no locking (8.2).
The bridge between the two is `TopExp::MapShapes(shape, TopAbs_EDGE,
map)`, which is exactly what `TopoShape`'s own cache uses, so index *n*
in the worker's map is `Edge<n>` in the element map -- no `TopoShape`
API is touched off the main thread.

**Evidence.**

    plain box            : 12 projected edges, 12 distinct sources
    box + slot (planar)  : 25 projected edges over all 24 edges, 0 unnamed
    box + cylindrical    : 5 unnamed, all of them the hole's silhouettes
    length agreement     : 9 agree, 0 differ
    bar behind a block   : Edge9;...;HLR:VH:0, HLR:VH:1, HLR:HH:0
    section              : SectionPlane;:G6;CUT;:H,E;HLR:VH:0
    after a model edit   : 9 of 9 names unchanged

The all-planar case is the one that settles what "unnamed" means: a
solid with no curved face leaves *nothing* without a source, so the
projected edges that have none appear only where a curved face does.
A silhouette is an edge HLR invents; it has no source edge, and a
cylinder's is a straight generatrix, not a curve -- which is why an
earlier probe that expected the unnamed ones to be curved reported a
false failure.

The length check is the one that shows the reported source is the right
one and not merely a plausible one: for every named edge of a view, the
projected length times the scale equals the source edge's, 9 of 9.

**Gates.**  TechDraw 11 of 11 (28.0 s); `ctest` 477 of 477; the Python
suite 1338 tests, 6 failures and 8 errors, this box's known set.

**What T2 does not do**, and does not claim to.  Vertices and 2D faces
still take no name: 3.6 has vertices naming from the edges that meet at
them and faces from the wires that bound them, and the second is the
change that kills the area-sort fragility.  Nothing persists the name
yet either -- `BaseGeom::Save` writes `ref3D` and not the strings, so
old files still restore unchanged.  Both belong with T3, which migrates
the consumers (`References2D`, `DrawHatch::Source`,
`GeomFormat::m_geomIndex`, `CenterLine`, `CosmeticVertex::linkGeom`) and
is where a stored name first has to survive a reload.

### 8.6 T3, built: the consumers keep their references by name (2026-09-07)

Four commits and one repair, in the order 3.6 gives.  The first finishes
the naming T2 left half done; the other three migrate the consumers, and
the repair is a crash the first probe to read a cosmetic from Python fell
into.

#### 8.6.1 Projected vertices and faces are named too

`ccef54d5f8`.  T2 named the projected edges and stopped there.  3.6 says
where the other two kinds get theirs: a vertex from the edges that meet
at it, a face from the wires that bound it.

`GeometryObject::nameVertexGeometry` runs beside `nameEdgeGeometry`, on
the main thread and for the same reason -- it reads names the edges
already carry.  A vertex takes the sources of every visible named edge
that starts or ends there, sorted and without duplicates:

    <sourceName>|<sourceName>|...;HLRV:<ordinal>

A centre mark meets no edge; it belongs to the circle whose centre it
is, so it takes that one source and `;HLRC:` instead.

`nameFaceGeometry` runs from `onFacesFinished`, because face finding is
a worker of its own.  A face's bounding fragments are **not** the
projected edges: `DrawProjectSplit::scrubEdges` fuses copies of them and
splits them where they cross, so each fragment is matched back to the
edge it lies on by its midpoint, against the same edge list the finder
was given, with a bounding box test to keep that to a few candidates.
The face name is the sorted distinct sources of those edges plus
`;HLRF:<ordinal>`.  `DrawViewPart.getVertexNames()` and `.getFaceNames()`
read both back.

Evidence, on a stepped block, a drilled plate and two disjoint blocks in
one view:

    vertices, stepped block : 6 of 6 named, all distinct
    vertices, drilled plate : 7 of 7 named, one of them ;HLRC:
    all-planar solid        : no vertex left without a source
    faces                   : every face named, and every source a face
                              names is one its own edges report
    the area sort           : growing the small block past the big one
                              swaps both face numbers; both faces keep
                              their names

That last line is the one 3.6 asked for.  A face's number is its rank in
the sort by area, which is the most fragile identity in the module; the
name does not move with it.

#### 8.6.2 A dimension's 2D reference

`6bbf82d4fc`.  The ladder 3.6 wants is *name -> exact geometry ->
similar geometry -> ask the user*.  Only the middle rung existed:
`SavedGeometry` kept a copy of each reference's geometry and, when it
stopped matching, the reference was repointed by searching the
projection for a shape that looked the same.

`DrawViewPart` gained the two directions of the bridge.
`getGeometryName()` turns a reference into the view -- `Edge3`,
`Vertex2`, `Face1` -- into the name that element carries, and
`getGeometryReference()` turns a name back into whatever number the
element has now.  The tag the name ends with says which pile to search,
so a name never resolves to an element of the wrong kind.  Both are on
`DrawViewPartPy`.

`DrawViewDimension` records those names in a new `SavedNames` property,
one entry per entry of `References2D`, and consults them first:
`fixByName()` repoints any reference whose name belongs to a different
element now, and `execute()` then runs the geometry ladder over what is
left -- which is the demotion 3.6 asks for.  Nothing here remembers
whether it has run, so the trap of 3.7, a migration spending its one
chance on the restoring pass, does not apply: while the view has not
projected there is no name to find and the answer is simply no.
`execute()` records the names of references it believes in, which is
what gives a document written before this change something to recover
with.

Evidence, with the geometry fallback switched off
(`Dimensions/AutoCorrectRefs`) so the name is the only rung that can
move anything.  Two blocks in one view, a dimension on the right-hand
block's 25 mm edge, and the edit slots the left-hand block, which comes
first in the projection:

    before        : Edge4, 25.000, name Edge1;:H926,E;HLR:VH:0
    after         : 12 projected edges where there were 8
    with the name : Edge8, 25.000 -- the element carrying that name
    without it    : Edge4, 3.000  -- an edge of the new slot
    saved, closed and reopened: the name comes back, still 25.000

#### 8.6.3 A hatch's face reference

`572035ebf0`.  `DrawHatch` and `DrawGeomHatch` point at a face by its
number, which 8.6.1 has just shown to be the least durable identity
there is.  Both gained a `SavedNames` property, recorded when `Source`
changes and consulted when the faces are next found.

The consulting is driven from the view, in `postFaceExtractionTasks`:
face finding runs in a worker of its own, so before it lands there is no
face to name and none to look up, and a hatch's own `execute()` runs too
early to see either.  The loop both of them run is
`DrawViewPart::geometryNamesOf` and `::repointByName`, the whole-link
form of the pair 8.6.2 added.

Evidence, two disjoint blocks in one view and hatches on the smaller
one's face, the edit growing it past the larger:

    before      : Face0 and Face1, all three hatches on Face0
    after       : the two faces have swapped numbers
    with a name : Face1, for the svg hatch and the geometric hatch alike
    without one : Face0, which is the other block now

#### 8.6.4 The cosmetic references

`432e9e4d0b`.  `GeomFormat` gained `m_geomName` beside `m_geomIndex`;
`CenterLine` gained `m_faceNames`, `m_edgeNames` and `m_vertNames`
beside its three reference vectors.  Both write the name **as an
attribute of the element that carries the reference** --
`<GeomIndex value="4" name="..."/>`, `<Face value="Face0" name="..."/>`
-- which is what keeps a document written before names existed reading
back unchanged: `reader.hasAttribute("name")` is false and the vectors
stay empty.  Appending a new element instead would have collided with
the lookahead `GeomFormat::Restore` already does for `LineNumber`.

`CosmeticExtension::syncGeomFormatNames` runs from `postHlrTasks`, where
the projected edges first exist; `CenterLineBuilder` records names when
the line is made and `addCenterLinesToGeom` repoints and re-records
before the line is rebuilt, which is after face finding.
`GeomFormatPy` gained `GeomIndex` and `GeomName`, `CenterLinePy` gained
`EdgeNames`, `FaceNames` and `PointNames`.

**`CosmeticVertex::linkGeom` is left alone**, and the header now says
why.  3.6 lists it with the others, but it is derived, not a reference:
`addCosmeticVertexesToGeom` rewrites it on every projection and nothing
reads the stored value back.  The cosmetic vertex is identified by its
tag, which does not move.

Evidence, in two documents -- one where the edges renumber under a
format, one where the faces swap under a centre line:

    format, before : Edge4, name Edge1;:H1072,E;HLR:VH:0
    format, after  : 12 edges where there were 8; the format is on
                     Edge8, which carries that name, and Edge4 is a
                     slot edge now
    centre, before : Face0
    centre, after  : the faces have swapped; the centre line is on Face1
    both survive a save, a close and a reopen

#### 8.6.5 The crash that reading a cosmetic from Python walks into

`3fc9fc9fba`, and it is not part of T3 -- it is what T3's probe found.

Reading a view's cosmetic list from Python and then closing the document
hangs the application, allocating without bound until it stops
responding.  `CosmeticVertex`, `CosmeticEdge`, `CenterLine` and
`GeomFormat` all cache the Python wrapper they hand out inside
themselves,

    if (PythonObject.is(Py::_None())) {
        PythonObject = Py::Object(new XPy(this), true);
    }

so the wrapper is kept alive by the object it wraps.  All four declared
`Delete="true"`, which makes the generated wrapper destructor delete its
twin (`templates/templateClassPyExport.py`).  Destroying the cosmetic
destroys the wrapper, which deletes the cosmetic that is already being
destroyed.  The list property owns these objects; the wrapper borrows
one.  `Delete="false"` on all four, and none of the types is registered
in a module, so there is nothing for it to leak.

    reading the list, then closing        : hangs at 1 GB, not responding
    closing without reading it            : closes
    with the fix, reading then closing    : closes

**This one cost most of the session's debugging time, twice over,** so
both traps are written down in 8.6.6.

#### 8.6.6 Traps this step paid for

- **A changed class layout needs a full build, not `--target TechDraw`.**
  Adding `std::string m_geomName` to `GeomFormat` moved `m_format`.
  `TechDrawGui` was not rebuilt, kept the old offsets, and wrote through
  them: `GeomIndex` read back as 875777200, OCCT threw "Illegal storage
  access" and then "Not enough memory available" from unrelated calls,
  and one probe hung.  None of it was a real defect.  On this box the
  rule is: **any header change under `src/Mod/TechDraw/App` means
  `cmake --build build/win-relwithdebinfo-801` with no target.**
- **A hang is not evidence until the baseline has been run.**  The
  wrapper-ownership hang looked like a pre-existing TechDraw bug, and
  the check that settled it was a probe using no new API at all
  (`fmt_cycle.py`: build, format, edit, save, close, reopen), run on the
  stashed tree.  The baseline passed, which is what turned "probably not
  mine" into "mine, and worth fixing".
- **Two `DrawViewPart`s on one page do not both finish finding faces**
  in a scripted run here -- the second view reports 8 edges and 0 faces
  indefinitely.  Probes that need faces on two views use two documents.
- **A restored `CenterLine` gets a new tag**, so `getCenterLine(tag)`
  does not find it after a reload; read it out of the `CenterLines`
  property instead.
- `makeCenterLine` with two parallel edges was seen to fail with
  `Illegal storage access` and `Not enough memory available` -- but only
  on the mixed-layout build above, and it was not retried afterwards.
  Treat it as unmeasured, not as a known defect.

#### 8.6.7 What T3 does not do

The recovery ladder's last rung, "ask the user", is still not built:
that is the repair dialog of 2.8, and 7.18 settled that it waits for the
upstream PartDesign and Sketcher merges.  `handleNoExactMatch` still
declares the references correct and moves on.

`BaseGeom::Save` still writes `ref3D` and not the names.  It does not
need to: the projected geometry is rebuilt on every recompute, and what
had to persist -- the name a *reference* was made against -- persists on
the referring object instead, which is where a reader will look for it.


## 9. Unstable indices in element names (handoff, 2026-11-12)

Next session's task, as the user put it: "take a deep look at the element
naming algorithm to reduce as much as possible the amount of unstable
indices in element names."  Nothing in this section is built.  It records
what was found and measured on 2026-11-12, an inventory of where a number
enters a mapped name, and where to start.

### 9.1 How it was found

The user's question, on taking OCCT `05c89fa705` (`ac01b0d63e`,
`BRepOffset_MakeOffset::JoinEdgePieces`: the pieces of an edge that the
second way to a thick solid left are joined into one): "why reference by
number when we have shadow element names for tracking".  A reference is
held by its mapped name, and the index is only a fallback.  But a mapped
name can itself hold a position, and then the name follows that position,
not the element.

Measured on realthunder/FreeCAD#360's plate
(`~/works/sw/occt/tests/fork/thickness/models/issue360_plate.brep`):
PartDesign Thickness, Face5 removed, 4 inward, Arc join, Intersection on.
The old OCCT, without the join, gives 642 edges and 496 vertices; the new
one gives 438 and 292, at the same volume, 38747.1395.  A reference was
made to every edge under the old library, the file saved, then opened and
recomputed under the new one:

| what the reference found | count | the old name |
| --- | --- | --- |
| the same edge | 344 | 284 by history, 60 `;:U` |
| missing (`?EdgeN`), shown as an error | 220 | 184 `;:U`, 36 by history |
| **another edge, no error** | **76** | **all `;:U`** |
| the joined edge that holds it | 2 | `;:U` |

No reference named by history ever landed on a wrong edge.  The 36
history-named ones that went missing were pieces whose joined edge took a
sibling's name.  A wrong edge is worse than a missing one: sec 7's stored
generations and `Part::Feature::getRelatedElements()` are asked only for a
name that is missing.  A name that resolves is trusted, and nothing
compares its geometry with the generation it was made against.

The name kinds on the plate's edges:

| | edges | `;:U<n>` | `;:M/G/MG<n>` | other |
| --- | --- | --- | --- | --- |
| without the join | 642 | 322 | 50 | 270 |
| with the join | 438 | 138 | 30 | 270 |

So even with the fix, a third of a thick solid's edges are named by
position.

This OCCT case did no harm to users.  The second way is unreleased
(`75bce7e802`, 2026-11-10), so no user's file holds those pieces.  The
weakness is general, though.  Any operation whose edges OCCT gives no
history for names them by position, and anything that changes how many
of them a face has re-targets their references: a kernel change, or the
user's own edit upstream.

### 9.2 Where a number enters a name

All of it is in `TopoShape::makESHAPE` (`src/Mod/Part/App/TopoShapeEx.cpp`,
from ~4412) unless noted.  The positions are from this tree, 2026-11-12.

1. **The history pass, `;:M<n>` / `;:G<n>` / `;:MG<n>`** (~4766).  When
   one source element is reported Modified or Generated into several
   results, each result gets the source's name plus `n`, its place in a
   `TopExp_Explorer` walk of the result (~4580).  There are two existing
   stabilisers: the first coplanar face gets `INT_MIN+1` (written `00`),
   and the first parallel face gets `INT_MIN` (written `0`).  These keep
   an extrusion's top and bottom stable when holes are added.  Every other
   case is positional.
2. **The reverse pass, `;:U<n>`** (~4786-4860).  An unnamed lower element
   is named from ONE named upper element, with `n` its place among that
   upper element's still-unnamed lower elements in `TopExp::MapShapes`
   order.  Where several uppers name it, the first in sorted order wins
   (`#ifndef FC_ELEMENT_MAP_ALL`).  This is the case measured in 9.1.
3. **The forward pass, `;:L(...)`** (~4862-4950).  An unnamed upper element
   is named from ALL its named lower elements, sorted, the first as the
   base and up to four more in the postfix.  No index, and stable while
   the lower names are.  But it applies only when every lower element is
   named (`names.clear()` on the first unnamed one), and the cap of four
   means two faces sharing the same first five sorted edge names would
   collide.
4. **Duplicates, `;D<n>`** (`ComplexGeoData::renameDuplicateElement`,
   `src/App/ElementMap.cpp` ~2329).  When two elements end up with one
   name, the later ones get the count.  This is order-dependent by
   construction (`FC_RANDOMIZE_DUPLICATE_INDEX` exists to flush out code
   that relies on it).
5. **`;:I<n>`**, a child's place in a compound or link array
   (`PartFeature.cpp` ~630, ~1508; `Link.cpp` ~1732).  This position is
   meant: the index IS the child's identity in an array.  Probably out of
   scope, but confirm.
6. **Inputs with no element map**: a plain `Part::Feature` shape, an
   import, a `.brep`.  Their elements are named `Face345` and so on, pure
   position, and every name derived from them inherits that.  Primitives
   were fixed by role (`docs/PrimitiveNames.md`).  Imports were not, and
   cannot be in general.  Unverified: the `#1f:159` at the head of the
   plate's names looks like such an input name held as a hasher id plus
   an index.  Check `StringID::toString` before relying on that reading.

### 9.3 Ideas to weigh, none ruled

- **Name a `;:U` element from every named upper it has, not one.**  An
  edge between two named faces would be named by the pair, with an index
  only among the edges that pair shares (usually one, so no index).  It
  then survives other edges on either face being added, removed or
  joined.  A vertex would take the same treatment with its edges or
  faces.  `FC_ELEMENT_MAP_ALL` already gives multiple names; look at why
  it is off before reusing it.
- **Order the `;:M/G<n>` siblings by something intrinsic**: their own
  lower-element names (as the forward pass does), or geometry (centre
  along the source's parameter), instead of explorer order.
- **Lift the forward pass's "every lower element named" condition**, and
  look at the cap of four.
- **A guard for what stays positional**: when a reference resolves by a
  name holding a position, compare its geometry with the stored
  generation (sec 7).  If it differs and exactly one other element
  matches, the reference was re-targeted.  Show it the way the found-again
  references are shown (`Gui/Utils.cpp` `RefoundReference`), never
  silently.  `13266c5b84` on thundereal/LinkVibe (not merged here) adds a
  one-match geometry search at 1e-5; build on it, do not add a second one.

### 9.4 Constraints that bind any change

- **The element-map naming is released** (Link builds have used it for
  years).  A changed algorithm changes the names of existing files'
  elements, and their references go missing on the first recompute.  The
  user's rule (2026-11-11, sheet metal memory): an automatic fix is safe
  only when the shape is unchanged, and at load it is (the stored shape),
  so a geometry search can carry old names over silently.  Anything else
  is marked and fixed visibly in the edit panel.  Whether a naming change
  needs an element map version, and how old files are carried, is the
  user's call.  Ask in prose, with a recommendation.
- **Pass-on name steps are deliberate** (memory, ruled 2026-10-11): the
  plain tag step on a passed-on element stays, because colour tracing
  needs it.
- Sheet metal stores its own numbers (`WallNumbers`/`BendNumbers`, keyed
  by edge names via `edgeKeys()`).  Any rename moves those keys, and
  `SheetMetalNames.py` runs in every sheet metal tearDown to catch it.
- Tests first, with the before-score in the commit, and every gate run
  frozen and unfrozen (`~/.cache/lv-merge/gates-lv.sh`).

### 9.5 Reproducing 9.1, and the first step

Scratch in `~/.cache/sm-audit/`:

- `plate360-refs.py`: `REFS_STEP=make` builds the plate's thickness, gives
  an `App::FeaturePython` a `PropertyLinkSubListGlobal` to every edge, and
  saves `p360-refs.FCStd`.  `REFS_STEP=check` opens it, recomputes, and
  sorts each reference into same / missing / wrong / joined by the old
  edge's midpoint and length, split by name kind.
- `occt-nojoin/libTKOffset.so.8.0.1` is TKOffset built from
  `BRepOffset_MakeOffset.{cxx,hxx}` as they stood before `ac01b0d63e`.
  Load it with `LD_PRELOAD`.  `LD_LIBRARY_PATH` does not work, because
  FreeCAD's libs carry RPATH, which outranks it.
- `plate360-dump.py` / `p360-{old,new}.json` list each edge's name,
  length, ends and middle.

Mapped names cannot be compared as text across processes or documents:
`#7:2`, `:H110d` and the like are document-local hasher ids.  Compare by
saving and reopening, as `plate360-refs.py` does.

**First step:** generalise that harness before changing anything.  Take a
corpus: PartDesign's test models, the thickness suite's models, sheet
metal parts, a filleted and patterned body.  Make a reference to every
face, edge and vertex.  Then perturb something upstream that changes the
element count on the referenced faces: add or remove a hole in the
sketch, change a thickness or pad length, flip a fillet radius.  Count
same / missing / wrong by name kind (9.2's six).  That table is the
before-score.  The goal is to drive the "wrong" column to zero and shrink
the positional kinds.  Then take the kinds in order of their share of
"wrong".

### 9.6 The harness, built (2026-11-12)

`scripts/name-stability/`: `harness.py` (the library), `cases.py` (the
corpus), `run.py` (the driver FreeCADCmd runs) and `run.sh [out] [case]`.
It runs in a user home of its own; `NAMES_FREEZE=0` runs it with
`ImmutableShapeValues` off.

A case builds a model.  Every face, edge and vertex of every feature gets
a reference (`App::PropertyLinkSubList` on a holder object), one upstream
change is made, the document is recomputed, and each reference is sorted
by what it found.  **The truth is the geometry**: an element whose
geometry is in the new shape unchanged (a vertex's point; an edge's ends,
middle and length; a face's centre, area, box and edge count) is still
that element, whatever its name says.

| the old geometry | the reference | counted as |
| --- | --- | --- |
| is still there | names it | same |
| is still there | names nothing | lost |
| is still there | names another element | **WRONG** |
| is not there | names nothing | gone |
| is not there | names an element sharing ground with it | follows |
| is not there | names an element sharing none | elsewhere |

`elsewhere` is not a verdict.  Where the change moves an element, nothing
here knows where it went, so the corpus keeps to changes that leave most
of a model where it was: a hole more, a feature more, one tool moved.  The
score is `WRONG`, then `lost`.

Each reference is counted twice.  *The reference*: what the property
holds after the recompute, with everything that looks for a missing name
(sec 7's generations, `getRelatedElements()`) having run.  *The name
alone*: the old mapped name looked up in the new shape and nothing else.
The first is what the user gets inside one session; the second is the
naming.  In one session the two differ only in `lost`: the ladder found
every lost name again here, because the shape of the generation before is
at hand.  Across a save and a kernel change it does not (9.1's 220).

A name is sorted by its own step and by the nearest step in its chain
that holds a position:

| | the step |
| --- | --- |
| `U` | the reverse pass: one upper element and a place among its unnamed ones |
| `Mn` | one of several results of one source, with its number |
| `M1` | the number-less first of several (found by its numbered siblings) |
| `I` | a place in a pattern or an array, which is meant |
| `D` | one of several elements of one name |
| `L` | the forward pass: named from its lower elements, no number |
| `hist` | modified or generated, alone or at a fixed place (`0`, `00`) |
| `pass` | passed on with the plain tag step |
| `B`, `Bn` | 9.9's prototype: all its upper elements; `Bn` with a number |

The corpus is 17 models and 36 changes: plates with holes, fillets,
chamfers and drafts in chains; pockets on faces; linear, polar, mirrored
and multi-transform patterns; a revolution with a groove; a pipe; a hole
feature; sheet metal; Part workbench cuts, fuses, fillets, chamfers and a
thickness; #360's plate hollowed, and the same plate (a `.brep`, no
names) pocketed.  A body and a solid body are not scored: they show their
tip's shape under their own tag, so every name of theirs changes when a
feature is added (95 of 95 on the sheet metal case), which is another
matter than a number in a name.

### 9.7 The before-score (2026-11-12, `b6e7c69184`)

7336 references.  The same frozen and unfrozen, and run to run.

| the nearest step holding a position | same | name lost | **WRONG** | all |
| --- | --- | --- | --- | --- |
| none | 3367 | 225 | | 4030 |
| `U` | 1543 | 71 | **189** | 2160 |
| `I` | 410 | 4 | | 570 |
| `Mn` | 239 | 35 | | 414 |
| `M1` | 120 | 9 | **2** | 162 |
| all | 5679 | 344 | **191** | 7336 |

- **191 references name a wrong element, 189 of them through a `;:U`
  step.**  29% of all names stand on one.  117 are on #360's plate, 74 on
  models that started with names.  The other two are one edge cut in
  pieces whose order changed.
- No `D` name appeared at all, and no `;:K` number.
- The dress-ups are where a user meets it.  A fillet, a chamfer, a draft
  and a thickness report their faces modified and say nothing of the
  edges round them, so those edges are `Face;:U<n>`: a hole added to a
  sketch re-targets 8 of a drafted block's 102 references, a notch 39 of
  404 on an L-shaped block with three dress-ups.  In one case the fillet
  after the draft went invalid: its own edge references were among them.
- 139 of the 344 lost names are `L` names, all on the plate (9.8, 2).

### 9.8 What the deep look found

1. **The reverse pass names an element from one upper element and a
   number, where it has two or three.**  An unnamed edge lies between two
   faces, and both are named in nearly every case; a vertex is where three
   meet.  Counted on the corpus as built (`~/.cache/name-audit/sim.py`):
   of 402 `U` edges, 251 hold a number today; named by the set of their
   named faces, 324 are alone in their set and need none, and all 263 `U`
   vertices are.  The 78 that are not alone are on the plate, and have
   only one named face (2).  It is what `docs/PrimitiveNames.md` already
   does by hand: "an edge by the faces it is between, a vertex by the
   faces that meet in it".
2. **Faces the kernel gives no history for.**  On the plate, 78 of the
   thick solid's 116 side walls come back as new faces that
   `BRepOffset_MakeOffset` reports neither modified nor generated (the
   other 38 pass as they are).  They are the input's walls: same surface,
   trimmed.  With no history they are named last, by the forward pass,
   from their own edges -- so the name changes when any of those edges'
   names does (139 lost), and until then they are no upper element for
   the edges between them and the rim (those 78 edges in 1).
3. **Which of two results is the first.**  One source cut into pieces
   gives `;:M`, `;:M2`, ... in the order the kernel lists them.  A second
   groove on a turned part re-listed the pieces of a profile edge, and
   `;:M` became another piece.  Small here (2 wrong, 44 lost), and no
   cheap answer: a piece named by what is round it changes its name at
   every edit near it, where a number changes only with the order.  For
   an edge the order along the source's curve is intrinsic and could
   replace the kernel's; for a face nothing so plain exists.  Not
   prototyped.
4. **The same element passed on in one run and remade in the other.**
   What is left lost on the plate after 9.9 (116): a wall that the kernel
   passes untouched at one thickness and remakes, identical, at another is
   `pass` in one and `;:M` in the other.  An element remade with the same
   geometry and the same bounds could keep the passed-on name.  Not
   prototyped.

### 9.9 Two prototypes, measured

In `TopoShape::makESHAPE`, each behind an environment switch read once,
so one binary gives all four columns.  **Not committed**: the patch is
`~/.cache/name-audit/prototype.patch` (163 lines).

- **A, `FC_NAME_UPPER_SET`.**  The reverse pass already collects every
  named upper element of an unnamed lower one, then keeps the first.  A
  keeps them all: the first in sorted order is the base, the rest are
  hashed into the postfix as the history pass does its other sources, and
  a number is given only among elements with the very same set.
  `Face;:U<n>;FLT` becomes `Face;:B#<the others>;FLT`, or `;:B2#...` for
  the second of a set.
- **B, `FC_NAME_SAME_GEOM`.**  After the history pass and before the
  reverse pass: a face with no name yet whose surface is the very surface
  of exactly one input face (the same `Geom_Surface` handle and location,
  no tolerance involved) is that face modified; an edge on the very curve
  of one input edge, within its range, is that edge modified.  Pieces are
  numbered after the ones the maker itself reported.

| | references | **WRONG** | name lost | names with no position in them | `U`/`Bn` names |
| --- | --- | --- | --- | --- | --- |
| before | 7336 | **191** | 344 | 4030 (55%) | 2160 |
| A | 7386 | **121** | 334 | 5898 (80%) | 162 |
| B | 7386 | **75** | 233 | 4200 (57%) | 2040 |
| A and B | 7386 | **2** | 219 | 6052 (82%) | 8 |

(50 more references with either: the fillet that went invalid in 9.7
stays valid.)  A alone clears every model that started with names (74 to
2) and leaves the plate as it was (119); B alone clears the plate and
leaves the dress-ups; together 2 are left, both finding 3.  Of the 219
names still lost, 116 are finding 4, 45 finding 3.

The Part (444) and Sketcher (182) Python suites pass with both switched
on, and PartDesign (1048) but for two, `SheetMetalNames.py` in every
sheet metal tearDown included.  The two are
`TestNameEncoding.testForkFileKeepsItsReferences` and
`testUpstreamFileKeepsItsReferences`: a stored file (one this fork wrote,
one upstream 1.1.4 wrote) is opened, both pads of every body are
lengthened **before anything is recomputed**, and a fillet's and a
chamfer's edge reference must still be found.  Each holds a `;:U` name;
the new naming has no such name, and the edge has moved, so the geometry
cannot find it: `?Edge23`.  (With the first try of A, which still spelled
`;:U`, both passed -- by the old name resolving, right or wrong.)  This
is the case 9.10's item 3 turns on.

**An older file, measured** (`~/.cache/name-audit/migrate.py`): every
corpus model saved by the old naming with a reference to every element,
then opened with A and B on and every feature made again, nothing
changed.  1318 of 7103 names change, and all 7103 references name the
element they were made on: the shape is the same, so the search by
geometry finds each one (`auto change element reference`).

**The first try of A got 5 of those wrong, and the reason binds any
change to the naming.**  It still wrote `;:U` and `;:U2` where an element
has one named upper element only, and those spellings were, in the old
file, the names of other elements.  A name that resolves is trusted, so
the reference moved and nothing looked further.  **A new scheme must
never spell a name the old one could have given to another element**:
hence `;:B`, a postfix the old naming never wrote, and B's pieces
numbered after the kernel's own.

### 9.10 To rule

Nothing here is decided; the naming is released (9.4).

1. **A: the set name.**  Recommended.  It is where 189 of 191 come from,
   it is general (any operation, any kernel), and it is the rule the
   primitives already follow.  Open in it: the postfix letter (`;:B` is
   the prototype's); and whether a vertex is named from its edges, as
   now, or from its faces directly -- shorter, and free of an edge's tie
   number, but a wire has no faces.
2. **B: history from shared geometry, or the same in the kernel.**  B is
   in FreeCAD and covers every maker that remakes a face on its surface.
   The other way is to make `BRepOffset_MakeOffset` report those walls
   (the fork's own second way, `75bce7e802`, is where they are remade),
   which mends one maker and keeps FreeCAD out of guessing.  They do not
   exclude each other; B first is the recommendation, because the
   handle test cannot be wrong about which surface a face lies on, and
   it is measured.
3. **How an older file is carried.**  Three ways, the first
   recommended.
   - *The version, and the prompt.*  `OpCodes::Version` 15 to 16, the
     fork's way for names "of an older making" (`docs/PrimitiveNames.md`
     sec 5): the file asks for its migration recompute at load, and on it
     every reference follows by geometry, as measured above.  Not
     covered, as it never was: a model edited before that first
     recompute -- the two tests in 9.9, which would then recompute before
     they edit, as the prompt has the user do.
   - *The old name kept as a second name.*  The element map holds more
     than one name for an element, and saves them.  The reverse pass can
     still compute `Face;:U<n>` and give it to the element behind the new
     name, and a reference that resolves through it is rewritten to the
     first name.  No prompt, and the two tests pass as they are.  The
     cost: the map grows by one name for each such element (about a sixth
     of all names here), the old pass stays in the code for good, an old
     reference is as weak as it was until its first recompute, and it
     does not reach the elements B renames.  Not prototyped.
   - *A version per object*, the old naming kept for old objects: two
     namings alive for good.  Not recommended.
4. **Upstream.**  Upstream FreeCAD names by this same algorithm, so
   today a file moves between the two with its names intact.  After A
   and B a file from upstream is an older file (item 3), and a file
   saved here, recomputed upstream, gets upstream's names back and its
   `;:B` references go through upstream's own recovery.  If that matters
   more than the 189, A and B stop here; if A proves itself, it is worth
   offering upstream.
5. **Findings 3 and 4**: after the above, by their share (2 wrong, 161
   lost).
6. **A guard for what stays positional** (9.3's last idea) is still
   worth having for `Bn`, `Mn` and `M1` names, on `13266c5b84`.

### 9.11 Ruled and built (2026-11-12)

Ruled on 9.10: "A, suggest an opcode. B in fcad. Then fix occt history.
Bump element version."  All four are built.

**A: an element is named from all its named upper elements.**  The
reverse pass of `TopoShape::makESHAPE` no longer writes `;:U<n>`.  An
unnamed edge or vertex takes the first of its upper elements' names, in
sorting order, as its base, and the others go into the postfix:

    <first upper name>;:B[<n>][#<id of "(other|other)">];<op>;:H<tag>:<len>,<type>

The postfix is `;:B`, "boundary" (`Data::boundaryPostfix()`, the one place
that spells it).  With no hasher the others are written out,
`;:B(Face3;:G;DFT;:H1:7,F)`.  `<n>` is there only for the second and later
of several elements in the very same upper elements, two edges between
the same two faces.  A vertex is still named from its edges, not from its
faces: a wire has none.  `;:U` is still read -- an older file's names
resolve until the shape is made again -- and `Data::upperPostfix()` stays
for that.

**B: the same geometry is history.**  After the history pass, a face or
an edge of the result with no name, whose surface or curve is the very
one of exactly one input element (the same handle and location; an edge
also within that edge's range), is that element modified, numbered after
the pieces the maker itself reported.  Two inputs on one surface give no
name: nothing is guessed.

**The kernel says it too.**  `BRepOffset_MakeOffset::Modified` now answers
for the walls a thick solid makes again where a cut edge of the removed
face meets them (OCCT `Thickness.md` sec 69; the check is
`tests/src/Mod/Part/App/ThickSolidHistory.cpp`).  On #360's plate at 2, 3,
4 and 5 inward, 22, 56, 78 and 110 faces, and as many edges, lay on input
geometry with nothing told of them; none do now.  With it B has nothing
left to find on the plate, and the corpus scores the same with the older
kernel and B alone as with this one: B stays for every other maker.

**The element map version is 16** (`OpCodes::Version`).  A file of an
older naming asks for its recompute at load, as files of older primitive
names do.

| | references | **WRONG** | name lost | names with no position in them | `U`/`Bn` names |
| --- | --- | --- | --- | --- | --- |
| before (9.7) | 7336 | **191** | 344 | 4030 (55%) | 2160 |
| built | 7386 | **2** | 219 | 6052 (82%) | 8 |

The same frozen and unfrozen.  The two wrong are finding 3; of the 219
lost, 116 are finding 4 and 45 finding 3.

**Older files, measured** (`~/.cache/name-audit/migrate.py`).  The 17
corpus models saved by the old naming with a reference on every element,
opened, every feature made again as it is: 1318 of 7103 names change and
all 7103 references name the element they were made on.  Two faults came
out of the stored files of `TestNameEncoding`, and are mended:

1. *A reference already put right was looked up again by its number.*  A
   sketch keeps `Support` beside `AttachmentSupport` and copies one into
   the other.  On the migration recompute the pad under it listed its
   faces in another order than when the file was written; `Support` went
   from `Face3` to `Face2`, rightly, wrote that into `AttachmentSupport`,
   and that one was then looked up as the *old* `Face2`, a cylinder: "No
   planar face".  `PropertyLinkBase::_updateElementReference` no longer
   searches by position for a reference whose name already finds the
   element at the number it holds.
2. *A file upstream wrote was never asked.*  Upstream's format keeps a
   shape's map in a file of its own, read after the document's XML, so
   the version check in `PropertyPartShape::Restore` saw no map and took
   the shape for one with nothing to regenerate -- for any version, since
   that check was written.  With naming 16 the chamfer of
   `FaceRefs_v1-1-4.FCStd` lost its edge (`?Edge23`) at the first edit.
   The check is now made again in `afterRestore`, on the naming alone
   (`TopoShape::isOlderNamingVersion`; upstream's kernel and map format
   numbers differ from the fork's in every file and are let through as
   before).  **So every file written by upstream FreeCAD now asks for a
   recompute the first time it is opened here**, which is item 4 of 9.10
   as ruled.  A file this fork wrote in upstream's format (schema 4)
   always asked.

Not covered, as it never was: a model edited before that recompute.  The
two tests now recompute before they edit, as the prompt has the user do.

**The string table.**  An edge named from two faces holds the ids of both
faces' names, and both hold the sketch edge they came from: the same id
twice.  `StringHasher::compact` drops what a dropped string was built
from when "the table and that string" are all that hold it, counted as
two references, and a string named twice in one list holds three -- it
stayed in the file for good.  The count is now by how often the list
names it (`StringHasherTest.compactDropsWhatAStringNamesTwice`; the fault
is older than this work, lists of ten or less were never sorted out), and
A gives each id once.  What the names cost: over the 17 corpus files made
again and saved, the string tables are 98799 bytes where they were 56325,
and the documents' XML, which holds the maps, 1809427 where it was
1770636 -- 4% on the two together.

**A step that holds a string's number says its own length with it.**
`;:B#2f;CHF;:H6ed:a,E` is ten characters and `;:B#7;CHF;:H6ed:9,E` nine:
the same name in two files' tables.  It was so for `;:G#<id>` and
`;:L#<id>` before; `testAForkImportTakesItsStringsByContent` now compares
its one recomputed name without the lengths.  A shape imported into
another table and not recomputed keeps the length it had.  Not chased.

**Tests.**  C++: `makEChamferNamesAnElementFromAllItsUpperElements`,
`makESHAPENumbersElementsOfTheSameUpperElements`,
`makESHAPETakesAnInputElementsGeometryForHistory` (`TopoShapeEx.cpp`),
`ThickSolidHistory.aWallMadeAgainIsModified`, and the compaction case
above; the expected names of the chamfer, fillet, prism, offset and draft
tests are the new ones.  Python: `TestNameStability` (a hole added to a
drafted, filleted block moves no name; no edge is named from one face and
a number), `TestNameEncoding` as above.

**Suites** (Linux): the C++ tests 1622 of 1623 -- the one is
`GuiAssemblyWorkbench_tests_run`, which failed before this work too
(`test_flexible_body_playback`: no Chrono in this tree; and
`test_playback_speed`) -- and Python 5302 OK, both frozen and unfrozen;
the name corpus the same both ways.  The kernel's own suites are in OCCT
`Thickness.md` sec 69.

**Still open.**  Finding 3 (the order of `;:M<n>` pieces: 2 wrong, 45
lost) and finding 4 (a wall passed on at one thickness and made again at
another: 116 lost; the kernel says "unchanged" in one run and "modified"
in the next, OCCT sec 69) are not built.  The guard for what stays
positional (9.10 item 6) is not ruled.  Whether A is offered upstream is
not asked.

### 9.12 The rest, built (2026-11-12)

Ruled on 9.11: "The postfix is ok. Finish the undone items."  Findings 3
and 4 and the guard are built; the element map version is 17.  The branch
first took `thundereal/LinkVibe` (`948d1f4888`): the guard builds on its
search by geometry, and it held the same mend as 9.11's first fault, a
day older (`eb1a503501`), which stands in place of this branch's.

**Finding 3: a result is numbered among the results of the very same
sources.**  A history name held its element's place in the list the maker
gives for its first source, and, behind `;K`, the place in each further
source's list: `Top;:G3(DrillFace;K-2)`.  One result more in a list, of
whatever other sources, moved the numbers of all after it -- a third
drill renamed the edges of the first two.  Now the sources tell a result
apart first (the first, and up to four more, as the name spells them),
and a number is counted only among the results they leave together: a
cut's edge is `Top;:G(DrillFace;K-1)`, the one edge of that face and that
one, with no number.  `;K` keeps `-1` for a source that generated the
result and `0`, `00` for the two fixed places; it holds no place any
more.

**The pieces of an edge are numbered along its curve**, not in the order
the maker lists them: the first is `;:M`, the last `;:M-1`, and one
between them says how many there are, `;:M2-3`.  The first and the last
hold an end of the edge and keep their names whichever piece is cut
again.  One between the ends is told by nothing but its place, which is
another piece's as soon as one more is cut before it; with the count in
it, its name is then no name at all, the reference is looked for by
geometry as any missing one is, and is found where the piece is
unchanged.  (Numbered from both ends without the count, the corpus had
one reference on a wrong piece, a disc's rim when a fourth lug cut it
again.)  The results of anything else that the sources leave together --
the two halves of a face -- are numbered in the maker's order, as they
were: for a face nothing as plain as a curve's direction exists.

**Finding 4: an element made again as it was is named as one passed
on.**  A result with one source that modified it, on the very surface or
curve of that source at the same location and with the same bounds (edge
for edge, by curve and range), takes the names the source would pass on:
the source's names under its tag, with no step of the operation.  The
plate's walls are `Face2` at every thickness, where they were `Face2`
passed on and `Face2;:M;THK` made again.  A boolean does the same to what
it rebuilds and leaves as it was (`Edge3;:M;CUT` is `Edge3`).  A vertex
is as it was at the very point *and* as an end of one of its edges that
is as it was: the point alone would make a mirror's vertices in its plane
the input's.  And B (9.11) takes a vertex so too.  Not for an input with
no tag, a shape between two steps of one feature: its bare name is not
told from a number (`Wall1`).

| | references | **WRONG** | name lost | names with no position in them | `Mn` names |
| --- | --- | --- | --- | --- | --- |
| 9.11, after the merge | 7362 | **2** | 219 | 5962 (81%) | 560 |
| built | 7362 | **0** | 85 | 6257 (85%) | 204 |

The same frozen and unfrozen.  (The merge took 24 references out of the
corpus: a pattern leaves out an instance since `6681f43f36`.)  What is
still lost is lost to its inputs: a sketch whose geometry the change
numbered anew, a tool that became a compound, an edge whose neighbours
changed -- and the pieces between an edge's ends, by design.

**Older files** (15 to 17): the 17 corpus models as 9.11 has them, 1847
of 7103 names change and all 7103 references name the element they were
made on.  More references than before stand on a name that changes -- a
face a fuse rebuilt unchanged was `;:M;FUS` and is passed on now -- so
the recompute before any edit matters more than it did:
`TestLoft.testLoftThroughItsEndWritesIntoNoInput`, which lengthens the
pads of a stored file first, lost the loft's section and now recomputes
before it edits, as `TestNameEncoding` does.

**The guard** (9.10 item 6).  When an object changes its shape, every
reference into it that still resolves is checked against the element it
was on: that element, by its number in the shape before, is looked for in
the new shape by geometry (`Part::Feature::searchElementCache`, the
search a missing reference gets, LinkVibe's `13266c5b84`).  Where it is
still there, unchanged, as one element, and the name finds another, the
reference has been moved off it:

- nothing is changed in the recompute -- the shape changed, which is no
  ground for moving a reference unseen (ruled 2026-11-11); the reference
  goes on by its name;
- the report view says so, and the reference keeps where the element
  lies (`App::ShadowSub::elsewhere`, a name of the target's, saved with
  the reference as `elsewhere=`);
- `DocumentObject.getReferencesElsewhere()` lists them: (property,
  object, the element named, the element it was on);
- a dress-up's panel, opened, takes the element the reference was on and
  shows it as it shows one found again by name: the row marked and its
  tip saying what it was, the element selected in the view, a line in the
  panel; OK keeps it, Cancel leaves the reference as it was.

It is kept until the reference is set anew, or names that element, or
the element is gone.  Every name is checked, not only one that holds a
place: the first of several results is spelled as an only one is.  The
cost is one search for each reference at each change of what it refers
to.  Not built: the other panels (a sketch-based feature's face, a
pattern's originals) list such a reference as it is.

**Tests.**  C++ (`TopoShapeEx.cpp`):
`makESHAPENamesAnElementMadeAgainAsOnePassedOn`,
`makESHAPENumbersAResultAmongThoseOfTheSameSources`,
`makESHAPENumbersThePiecesOfAnEdgeAlongItsCurve`; the expected names of
ten boolean, slice and transform cases are the new ones.  Python:
`TestReferenceGuard`.  GUI: `dressup-moved-reference.py`.

**Suites** (Linux): the C++ tests 1623 of 1624 -- the one is
`GuiAssemblyWorkbench_tests_run`, as in 9.11 -- and Python 5326 OK, both
frozen and unfrozen; the name corpus the same both ways.  Three
expectations moved with the names: `TestTopologicalNamingProblem` counts
two names on a vertex made again as it was (the input had two), in a
pocket, a revolution and a fillet.

**Still open.**  The halves of a face keep the maker's order.  The guard
is shown in the dress-ups' panel only.  Whether any of this is offered
upstream is not asked.
