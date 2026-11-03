# Sheet metal workbench: design (2026-10-30, proposed)

Status: PROPOSED; M0's geometry layer and its join measurement built (section 12, Progress). The
open questions are section 12. The user asked on 2026-10-26 for
three things in the pending (end of 2026) release: a 3D sketch, a fuller Surface workbench and
sheet metal, all native C++ rather than bundled Python addons. The 3D sketch and Surface are done
(docs/Sketch3D.md, docs/Surface.md). On 2026-10-30 the user ruled: survey sheet metal against other
CAD programs, and build a native workbench if the SheetMetal addon is slow or limited in features.
The survey found it limited (docs/SheetMetalSurvey.md, "Verdict"), so this is the native design,
written before any code, as Surface's was.

The survey is cited as [A n] for its Part A sections (other CAD programs and upstream) and [B n]
for its Part B sections (the addon); [A src n] is Part A's numbered source n, and #n an addon
issue. Claims about our own code were read from the source on 2026-10-30. Nothing here is
measured yet: the addon's speed was not timed (running its code needs the user's leave, [B2]),
and the native builder's speed is section 9's first gate.

## 1. What it is for

Sheet metal parts are cut flat from a sheet of one thickness and bent on a press brake: brackets,
enclosures, chassis, ducts, panels. The model has to answer two questions at once:

1. what the folded part looks like, to fit it into an assembly;
2. what the flat blank is, exactly, to cut it on a laser or a punch, with the bend lines marked.

The second is the point. A solid that merely looks like sheet metal does not answer it. The flat
length of a bend depends on where the neutral fibre sits in the sheet (the K-factor) or on a
shop's measured table, and an error of a tenth of a millimetre per bend is a scrapped part.

Every CAD program in the survey has the same core [A7b]:
- **One rule set per part:** thickness, bend radius, K-factor or table, gaps, relief defaults.
- **Walls:** a plate from a closed sketch, a profile with bends from an open sketch, flanges on
  edges, a fold on a sketched line, a lofted flange.
- **Edges and corners:** hems, automatic bend reliefs, corner reliefs and corner closures.
- **Convert:** a solid or an imported thin part made sheet metal, with rips.
- **Flat:** a live flat pattern, and DXF of it with the bend lines on their own layer.
- **Multi-body parts.**

FreeCAD has none of this in core [A5]. The SheetMetal addon has the everyday part of it and lacks
the rest [B6]. Its author's own view of its main limitation is that it "works on 'solids' not
sheets": each operation reverse-engineers the sheet from the solid it is given [A5].

The goal: a sheet metal user can design a part in this fork without an addon, get a flat pattern
that is right to the bend table, and send it to a cutter; and the part is a PartDesign body like any
other, so holes, pockets, patterns and mirrors work on it.

## 2. What other CAD programs do

The full tables are [A1] to [A7]. What shapes this design:

- **The features know their bends.** SOLIDWORKS, Inventor, Creo, NX, Solid Edge, CATIA and Fusion
  are feature-based: a flange feature makes a bend and knows its radius, angle and allowance. The
  flat pattern comes from that knowledge, not from looking at the solid. Every one of them also
  recognises sheet metal in an imported solid, as a second path [A7b]. So a native design needs
  both: features that know, and a recogniser for what no feature made.
- **Ordinary modelling tools work on sheet metal.** A hole, a cut across a bend, a pattern: the
  mature programs let the general tools act on the sheet metal part, and the flat pattern carries
  their result (gk; Solid Edge adds features across bends by unbending and rebending [A6.7]).
- **The rule set is a part-level object** tied, in the mature programs, to a material and a gauge
  table, with overrides per feature and per bend [A3]. Onshape puts the per-bend overrides in a
  table beside the flat view, editable cell by cell [A4].
- **The flat pattern is live.** Either a flat state of the same part (most) or a permanent side
  view kept in sync (Onshape) [A4]. Refold -- unfold, cut in the flat, fold back -- is in eight of
  ten programs [A7a].
- **Bend allowance has a few standard models** [A2]: the K-factor arc `BA = pi * (R + K*T) * A /
  180`; bend allowance and bend deduction tables keyed on thickness, radius and angle; K by formula
  from R/T (DIN 6935, CATIA's default). Creo's Y-factor is K-factor in another form.
- **Forms are kept or marked.** A louver, dimple or emboss is either kept formed in the flat or
  shown as an outline or a centre mark [A2.2, last list].

## 3. What this fork has to build on

### 3.1 Nothing sheet-metal specific

No module in `src/Mod` handles sheet metal; the only match for "sheet metal" in `src/` is an
AddonManager resource [A src 61].

### 3.2 PartDesign

- **The Body** takes any `PartDesign::Feature` (`Body::isAllowed`, PartDesign/App/Body.cpp:241).
  A sheet metal feature derived from `PartDesign::FeatureAddSub` lives in a body, is a Tip, and
  works with every PartDesign feature before and after it.
- **`PartDesign::SolidBody`** (SolidBody.h) is a solid inside a body with its own history and tip.
  That is the multi-body case: one sheet per solid, each with its own rule set (4.6), shown as
  the solid body's first child.
- **The features a sheet metal part uses unchanged:** Hole (countersinks, which a flat pattern
  must keep), Pocket, Pattern, Mirror, Fillet and Chamfer on the flat corners (the "break corner"
  of [A7a]), Thickness (hollow a solid: the classic route to "convert"), and Thicken (a surface made
  a solid, added in the Surface work).

### 3.3 Part and the element map

- The `TopoShape::makE*` builders (Part/App/TopoShape.h) make every result with an element map:
  prism, revolution, booleans, offset, thick solid, thicken, fillet, slice, pipe shell, loft.
- **Names by role.** Primitives name their elements by what they are, `Top`, `FrontL`
  (docs/PrimitiveNames.md, `Part::PrimitiveNames`). A sheet metal feature can name its faces the
  same way: a flange's bend `Inner` and `Outer`, its leg's two skins and its end. The cost of
  names was measured then: noise in a long history, about 380 B a name on a primitive
  (PrimitiveNames.md sec 3).
- **The addon already leans on this fork's naming.** Its author: "on realthunder's FreeCad version,
  everything works fine. no TNP issues with SheetMetal" (#342) [B3]. A native feature gets the same
  remapping directly, with names it chose instead of anonymous boolean results.
- **No boolean glue option is wired in Part.** OCCT's Boolean algorithm can skip the
  face-against-face tests when the arguments only touch along shared faces
  (`BOPAlgo_GlueShift`/`GlueFull`, gk). A flange touching its wall is that case. Nothing in
  `src/Mod/Part/App` sets it today (grep 2026-10-30); section 4.4 may need it.

### 3.4 Elsewhere in the tree

- **DXF:** the C++ writer in `src/Mod/Import/App/dxf/` has layers (`CDxfWrite::setLayerName`),
  lines, arcs, circles, splines and text (dxf.h:372-402). That is all a flat pattern needs: the
  outline, inner contours and bend lines each on a layer, labels as text. The addon goes through
  Draft's Python exporter instead [B2, "Export"].
- **Materials:** the Material module has models in YAML, including ones that hold arrays
  (`Models/Mechanical/LinearElasticArrays.yml`). No sheet metal model exists. A model with
  thickness gauges, default radius, K-factor and a bend table is a natural fit (question c).
- **Surface:** docs/Surface.md sec 5.4 deferred Flatten to sheet metal. The development of a
  cylinder and a cone, which a bend unfold needs, is the same maths.
- **Out of process:** docs/ComputeBoundaries.md is "design notes, not yet implemented". The flat
  pattern is kept a pure function of its inputs (section 4.5) so it can move to a worker when that
  exists; this design builds no worker.
- **The OCCT fork's offset work** (thickness fixes behind opt-in levels) is the kernel path that
  "convert a shell to sheet metal" and forming tools use.

### 3.5 The SheetMetal addon

LGPL-2.1-or-later, so its algorithms, or its code translated, can be reused with the notices
kept [B0]. About 17,800 lines of Python, 19 commands [B0][B1]. What this design takes from it is
the tool list and its parameters [B6, "Day-one core"], which its users rely on daily, and the
lessons of its tracker. Not its structure: every feature copies the whole solid, re-runs booleans
on it and finds faces by booleans, so a chain of N features is O(N^2) and an edit at the start
re-runs everything [B2, "Complexity"]. Its default unfolder flattens one skin only, which loses
countersinks and forms (#493, #563) [B2].

Its K-factor table interpolation is wrong past the table's second row: with the README's own table,
r/t = 50 gives K = 1.605 [B2]. A native flat pattern that is right will differ from the addon's on
such parts. Section 9 compares against hand-computed flats, never against the addon's output.

**Its install shadows a built-in module of the same name.** `FreeCADInit.py` keys modules by
lower-cased directory name, and the user's Mod directory overrides the built-in one (lines
118-127). The addon installs as `Mod/SheetMetal`, so a native module in `src/Mod/SheetMetal` would
not load at all for anyone with the addon. Question a.

## 4. Architecture

### 4.1 The part is a solid; sheet metal adds ownership

The central decision (question b). The proposal:

- **The part is the PartDesign solid,** as for any body. Every feature, sheet metal or not,
  produces it.
- **Sheet metal adds a sheet model beside it.** The model is a set of **elements**, walls and
  bends, and every face of the solid belongs to exactly one element.
- **Each element knows how it folds:** a map between its place in the folded part and its place
  in the flat.
  - A **wall** is a slab between two parallel planes; its map is rigid.
  - A **cylindrical bend** is a sector of a tube, inner radius R, thickness T, angle A; its map
    unrolls the angle onto a strip of width `(R + K*T) * A`, taking each point at depth d below
    the inner skin to the same depth in the flat.
  - A **conical bend** (a tapered or lofted flange) is the same about a cone, developed exactly.
- **The flat pattern is every face of the solid sent through its element's map.** Both skins and
  the thickness faces go across, so a countersink, an emboss or a chamfer stays what it is. In a
  wall the map is rigid, so the faces stay exact. In a bend the map is exact for a skin face (an
  arc unrolled to a line) and for a cut normal to the sheet (its boundary is a curve on the skin,
  carried across, then extruded). Anything else in a bend is approximated within tolerance and
  reported (risk 2).
- **Refold is the inverse map.** A cut drawn on the flat goes back through each element it
  crosses and is cut from the folded solid.

The alternative is a sheet representation as the primary model (the addon author's view in PR
#31913 [A5]): walls as 2D regions, bends as strips, the solid built from them at the end. Flat and
refold become trivial. The cost is that every PartDesign tool -- hole, pocket, pattern, mirror,
fillet -- needs a sheet metal twin, because a general feature cannot edit the region model. The
proposal keeps the solid primary, and keeps the model as knowledge about the solid.

### 4.2 Where the model comes from

The model is computed with the solid and stored with each sheet metal feature, as a hidden
property. Three ways an element and its faces come to be known, in order:

1. **Made by a sheet metal feature.** The feature creates the element (it knows its planes, axis,
   radius, angle) and names the faces it makes by role (4.3). Exact; no search.
2. **Carried by name through a non-sheet feature.** A Pocket after a flange is not sheet metal.
   Its result's faces still trace, through the element map, to the faces they were made from:
   - a wall's skin that the pocket trimmed is "modified" from that skin;
   - the pocket's own side faces have no sheet metal ancestor, so they are placed by where they
     lie: a point of the face tested against each element's volume (a slab or a tube sector).
   The next sheet metal feature, or the flat pattern, takes the model from the last sheet metal
   feature before it and brings it up to date this way.
3. **Recognised from geometry** when nothing named it: an imported STEP part, a solid from Part,
   the "convert" route. Thickness is found from pairs of parallel planes and coaxial cylinders,
   walls and bends from a graph of tangent faces (the addon's V2 method [B2], but on both skins),
   and the result is checked: every face owned, the thickness constant within tolerance. A part
   that fails the check says which face and why.

So a hole or pocket in a sheet metal body works with no sheet metal code: way 2 places its faces
in the flat. A body with no sheet metal feature at all can still be flattened: way 3.

### 4.3 Names

A sheet metal feature names what it makes by role, as primitives do. For example:
- a bend's `Inner`, `Outer` and its two ends;
- a flange leg's `Skin1`, `Skin2` and `End`;
- a relief's faces.
The bend's own name, and the edge it was made on, go into the names. That is what lets:
- a later flange pick "the end of flange 2" and keep it across an edit;
- the flat pattern's bend lines and labels keep their names, so a drawing dimension to one
  survives;
- the per-bend overrides (4.6) attach to a bend that keeps its name while flanges are added
  before it.

### 4.4 Building the solid without booleans as search

The addon's cost is booleans used to find faces, and whole-solid booleans per flange [B2]. Here:
- **The feature never searches.** It reads the element it attaches to from the model: the edge's
  wall, its planes, which side is the inner skin.
- **The new material is built directly:** a bend is a revolution of the wall's end face, a leg a
  prism of its outline, a relief a prism cut into both. Each is named as it is made.
- **It joins the part along the faces they share.** A flange touches its wall only along the
  wall's end face, so the join is a boolean whose arguments only touch. Whether OCCT's glue option
  makes that cost nearly nothing on a large solid, or a direct replacement of the one end face is
  needed, is measured in M0 before the flange is built (section 9).
- **Many flanges in one feature are built and joined in one pass,** as the addon's "fan" case is.

Whether a chain of N features is still O(N^2) depends on that join: a boolean visits the whole
solid at least once. The target is set in M0 from measurement, not assumed.

**Measured 2026-10-30** (`scripts/sheetmetal-join-probe.cpp`, plain OCCT; and the same through
`TopoShape`'s element-mapped fuse, `~/.cache/lv-merge/sheetmetal/join/mapped.py`). The part is a
plate 2 thick with N flanges along one edge, each a 90 deg bend of inner radius 2 and a 20-high
leg. Every result is one valid solid.

| N | faces | chain, plain | chain, glue | chain, named | last join, named | fan, plain | fan, glue | fan, named |
|---|---|---|---|---|---|---|---|---|
| 16 | 166 | 118 ms | 97 ms | 172 ms | 15 ms | 47 ms | 28 ms | 46 ms |
| 64 | 646 | 1.26 s | 1.14 s | 1.76 s | 51 ms | 211 ms | 127 ms | 224 ms |
| 128 | 1286 | 4.9 s | 4.7 s | 7.7 s | 127 ms | 517 ms | 293 ms | 543 ms |
| 256 | 2566 | 24 s | 22 s | -- | -- | 1.36 s | 675 ms | -- |

What it says:
- **A chain is O(N^2), and the glue option does not change that.** One join costs about 0.07 ms
  per face of the result (0.08 ms with names), because the Boolean walks the whole solid. Glue
  saves little in a chain, since the cost is the walk, not the intersections. Names add about 40%.
- **A fan is close to linear, and glue halves it.** 64 flanges in one feature take 0.13 s.
- **This is not particular to sheet metal.** Every PartDesign feature on a solid of 650 faces
  pays the same walk: a pocket there costs about as much as a flange.

So, ruled by measurement, not proposed: features join with an ordinary named boolean, with the
glue option when the new material only touches the part (a flange, a hem, a tab). A feature on
many edges joins all of them in one boolean. Replacing faces directly in the part is not built:
at 51 ms for the 64th feature, it would win too little for the risk of building topology by hand.
Two things remain if large parts need them: a recompute that starts at the edited feature, which
is the recompute engine's work, and docs/ComputeBoundaries.md's workers. The M1 target is a
64-flange chain rebuilt in under 2.5 s, and one flange added to it in under 80 ms, through the real
features, element maps and all.

**Measured against it 2026-10-31** (`scripts/sheetmetal-chain-probe.py`, M0's part through the real
features, one per flange, frozen / unfrozen): the 64-flange chain rebuilds in 3.47 s / 4.54 s, and
a 65th flange adds in 140 ms / 220 ms -- **the target is not met**. Of the last flange's 125 ms,
87 ms is its one join with the 646-face part, 6 ms the join of its own bend and leg, 4 ms setting
the shape, the rest the document's recompute. The join costs more than M0's 51 ms for the 64th
named join; that, glue for material that only touches, and a recompute that starts at the edited
feature are what is left to look at. **Ruled 2026-10-31: accepted for M1**; glue and a recompute
from the edited feature are for a speed pass after it.

**The speed pass, 2026-11-01** (the probe, plus the last flange recomputed 15 times; each change
measured against the others switched off in one binary, runs interleaved, as this box's wall clock
drifts by 20% with what the Windows host is doing). Profiled with perf's DWARF call graphs: the
joins were not the walk alone.
- **Pcurves on planes, stored.** OCCT keeps no curve on a plane for an edge built on one, and works
  it out on each request (`BRep_Tool::CurveOnPlane`, a projection); the boolean's post-treatment
  (`CorrectWires`, over every face of the result, the unchanged ones included), its solid builder
  and the orientation check asked for every face of the part each join. The sheet metal features
  now store them on what they add and cut (`SheetMetal::storePCurves()`), so the part carries them
  down the chain. The file is unchanged: the BREP writer leaves out a plane's curve it can work out
  again exactly.
- **No orientation check after the join** (`makeBoolean(base, tool, oriented)`): a classification
  over every face of the result, where a boolean of oriented solids is oriented. The sheet metal
  features orient their own small tool instead.
- **No copy of the part in the body.** `Body` and `SolidBody` baked the tip's placement into the
  geometry with a copy of every face on every recompute, an identity placement included; a tip
  with no location is now taken as it is (every PartDesign body, not sheet metal only).
- Glue measured again and dropped: 3 ms of a 62 ms boolean, all of it in the intersection.

Frozen: the rebuild 3.42-3.70 s -> 2.62-2.82 s, one flange 124-130 ms -> 78-81 ms (median
155 -> 84). One more flange meets the target; the rebuild is within 10% of it. Of the 80 ms, about
50 is OCCT's boolean (the intersection, the solid rebuilt from its faces, the post-treatment), 10
the element map. Not frozen, nothing changes (4.4 s, 230-250 ms): the transaction log writes each
value at commit on the main thread there, by design, and the writer's test of a stored plane
curve, answered once for a frozen edge, runs on each write of an unfrozen one. Tested by
`testPCurvesStored`, `testBodyTakesTheTipAsIs`, `testLocatedTipIsBaked`.
- **The boolean's post-treatment** (OCCT f6cbbbd901, TKBO): it checked the tolerances of every
  face and edge of the result and then refused to change any of an argument's, which in a
  non-destructive boolean is nearly all of them; it now leaves those out. No result changes (every
  write went through the one refusal; the fork's suites as before, frozen and unfrozen). The
  library swapped between interleaved runs: rebuilt 2.62 -> 2.48 s, one more flange 79 -> 75 ms
  (median 85 -> 79). **Both targets are met, frozen.** Left: a recompute that starts at the edited
  feature.
- The first measurement was 26 s: the flange moved the base by its location and had its placement
  applied to its shape, and a location made of an identity placement is not an empty one, so every
  flange stacked another and every boolean keyed the edges' curves on their surfaces to it
  (fixed, f3146520da: the flange is built in place, as a pad of the current version is).
- PartDesign's own primitives pay the same, measured: 64 additive boxes along a plate's edge
  (326 faces), the last box 1067 ms, a box fused onto the result 1276 ms, the rebuild 17.3 s, where
  64 pads making the same part take 78 ms and 71 ms. Dress-ups apply their placement the same way
  and are likely to; not measured. **Ruled 2026-10-31: fixed as a change of its own**, with its
  own gate round. Done (2f68a3aeca): `Part::Feature::getLocation()` makes one location for one
  placement, kept, and none for the identity, and applies that one to its shape;
  `TopoShape::setTransform()` makes none of the identity. The 64 boxes now: the last 70 ms, the fuse
  58 ms, the rebuild 1.87 s, as the pads. Dress-ups measured too (a chamfer chain, which stacked one
  datum per chamfer). Tested by `TestChainLocations`.
### 4.5 Two layers, as in Surface

- **`App/Geom/`: geometry, no document.**
  - The sheet model (elements, their maps, face ownership).
  - Bend allowance (4.6).
  - The flat builder: a solid and a model in, a flat solid and its bend lines out, no document.
  - The recogniser.
  - The DXF writer of a flat.
  - Each is tested by GoogleTest on known geometry, and exposed to Python as `<Module>.Geom`.
  - The flat builder is a pure function of its inputs, so it can run in a worker process later
    (3.4).
- **`App/Feature*`: parametric features**, derived from `PartDesign::FeatureAddSub` (or `Feature`
  for the flat pattern), tested from Python.

Gui commands and task panels sit on top. The panels are written for the browser from the start,
as Surface's were after their first round (docs/Surface.md sec 12, fb9aa6ee53).

### 4.6 The rule set and bend allowance

- **A rule object, `PartDesign::SheetMetalRule`** (ruled 2026-10-31), holds the values a sheet's
  features share. It is a `Part::Feature`: its material card is the `ShapeMaterial` every Part
  feature has, and its shape, empty for now, is kept for later use (a test coupon at the rule's
  thickness and radius, the stock blank for nesting).
  - The properties: `Thickness`, `BendRadius` (inner), `MinFlange`, `AllowanceMethod`, `KFactor`,
    `Gap` (between walls meeting at a corner), the bend relief defaults `ReliefShape`
    (rectangle, obround, tear), `ReliefWidth` and `ReliefDepth`, the corner relief defaults
    `CornerReliefShape` (none, circle, square, tear) and `CornerReliefSize`, and the hidden
    `Overridden` (below). About fifteen as the later phases add hem and gusset defaults.
  - **Each sheet metal feature links it** (`Rule`). The first feature of a sheet makes one; every
    later feature takes the link of the feature it builds on, so nobody sets it by hand.
  - **The features depend on the rule, never the other way round.** A rule value change
    recomputes them as any edit does; `=SheetMetalRule.Thickness` works in any feature; a
    spreadsheet or a VarSet drives the rule as it drives anything. Rejected for this: the rule
    on the SolidBody, which depends on its features. A feature cannot refer to it by expression
    (a cyclic reference, measured); marking the values `Property::Input` lifts that but lets
    only input properties drive them, which a spreadsheet cell cannot be, as the spreadsheet
    writes its cells in its own recompute (measured 2026-10-31). Rejected too: the rule on the
    sheet's first feature, which makes the first feature special.
  - **One rule may serve several sheets**, or several bodies, the usual shop case of one stock
    and one material; the `Rule` link reaches outside the body for that (a global link, which
    PartDesign's links otherwise are not), and across documents.
  - **In the tree** the rule is the first child of each SolidBody whose sheet uses it, claimed by
    the view provider and not a member of the SolidBody's `Group`, which keeps the solid's
    history. So a solid holding a sheet metal feature always has a SolidBody, even when it is the
    body's only solid; in a body with `AddSolidBody` off (a document from before SolidBody), the
    first sheet metal feature turns it on, which groups the body's solids the new way from then
    on. The body's Misc group leaves a rule to its solid bodies.
  - Deleting a rule that features still link asks first, as deleting a sketch a pad uses does;
    copying features brings the rule with them by the usual dependency prompt.
- **Allowance methods:**
  - K-factor, either direct or by DIN 6935 from R/T;
  - a bend allowance table and a bend deduction table, each keyed on thickness, radius and angle
    and interpolated linearly in each;
  - the Y-factor read as `K = Y * 2 / pi`.
  The tables are interpolated correctly, the addon's case [B2] being a test.
- **The value for one bend is taken from the most specific source:**
  1. the bend's own override;
  2. the feature's;
  3. the table, by the bend's material, thickness, radius and angle;
  4. the rule's default.
  The source of each bend's value is shown in the flat pattern's bend table, so nobody has to
  guess which one won.
- **Material** (question c, ruled 2026-10-30: from a card): the rule may take its thickness gauges,
  radius and K or table from a material card with a sheet metal model, a new model in the Material
  module, or carry its own values when no card is chosen. Proposed:
  - The model, `SheetMetal`: a gauge table (a 2D array, one row per stock thickness: thickness,
    inner radius, K, minimum flange), the allowance method, and the bend table for the table
    methods.
  - The card is the rule's own material (`ShapeMaterial`), which follows that of the body holding
    the rule until the rule is given another, as the body's features do. So mass and the flat
    agree.
  - With a card holding the model, the rule's `BendRadius`, `KFactor` and `MinFlange` are the
    card's gauge row for the rule's `Thickness` (within 1e-6 mm), and the allowance method and
    table are the card's. The thickness itself and the gap and relief values are always the
    rule's. A value set
    by hand overrides the card's and is remembered as overridden (`Overridden`, the names), so a
    change of card does not undo it; `resetRuleValue(name)` gives it back to the card. A thickness
    with no row in the card's gauges keeps the rule's own values, and says so.
  - Without such a card the rule's values are its own. The defaults: 1 mm thick, 1 mm radius,
    K 0.5 by the KFactor method, no minimum flange, a 0.1 mm gap, a rectangular bend relief
    1 mm wide and 1 mm deep, a circular corner relief of 2 mm.
  - The table methods take their table from the card; the rule holds no table of its own.

### 4.7 The flat pattern

- **A FlatPattern feature** per sheet, in the body but not in its solid chain (like a sketch).
- Its shape is the flat solid, laid on the XY plane from a fixed face the user picks (the root),
  the bend lines as edges in a compound beside it, each named after its bend.
- **It is live:** it recomputes with the part. If the flat cannot be built (a face no element owns,
  overlapping material in the flat), it says which face or bend, and keeps the last good flat
  marked stale.
- **Its panel has the bend table:** each bend's angle, radius, allowance, value source and
  direction (up or down), editable where an override is allowed, picked faces cross-highlighting in
  both the folded and the flat view (Onshape's model [A4]).
- **Export:** DXF with the outline, inner contours, bend lines (up and down on separate layers)
  and labels on their own layers, from the C++ writer; SVG the same; TechDraw takes it like any
  shape.
- **Forms:** each form (4.8) is kept formed, drawn as its outline, or drawn as a centre mark, by a
  choice on the flat pattern.

### 4.8 Forms

A form (a dimple, louver, bead, lance, drawn cutout, flanged hole, emboss, gusset) is a tool body
pressed into a wall, a gusset into a bend. It is one element of
its own, owned by the wall it sits on, carried rigidly into the flat and drawn there as 4.7 says.
User tools are any solid with a marked face; a built-in set is parametric (question f, ruled
both): dimple, louver, bead, lance, and from the audit of 5.7 drawn cutout, flanged hole (a collar
to thread), emboss and gusset.

## 5. The tools

"Ref" is the addon command that does something similar [B1 row], consulted for its parameters and
its tracker, not ported.

### 5.1 Walls

| Tool | What it does | Ref |
|---|---|---|
| **Base wall** | A closed sketch makes a plate; an open sketch makes a profile wall with a bend at every corner, extruded by a length or symmetric. Material inside, outside or mid-plane of the sketch. | B1 3 |
| **Flange** | One or more edges of walls: angle (or up to a face, or parallel to one), length measured as leg, tangent, inner sharp or outer sharp, bend position (material inside, outside, bend outside, offset), gaps and extents at each end, partial width, automatic miter where two flanges in the feature meet. A closed sketch on the end makes the leg's shape. | B1 5 |
| **Contour flange** | An open chain of lines sketched square to an edge from it, swept along one or more edges: a bend where it leaves the wall and at each corner, the gaps, relief and miters as the flange's. | B1 5 (profile mode) |
| **Hem** | Flat, open, teardrop, rolled. | B1 6 |
| **Fold** | Bends a wall along a sketched line: radius, angle, which side moves, the line at the bend's start, middle or end. | B1 9 |
| **Jog** | Two opposite bends along a line, by an offset, the far end kept or the material taken from the wall. | -- (SOLIDWORKS Jog) |
| **Tab** | A closed sketch on a wall's skin adds a plate on that wall. | B1 8 |
| **Extend wall** | A wall's end face pushed out by a length, or up to a face, with gaps and a clearance against walls it would meet. | B1 7 |
| **Bend between walls** | Two edges picked, one at each wall's end, joined by a bend of the rule's radius; the walls trimmed or extended to meet it, over the length the edges share. Where two sheets of a part come together. Built in M2 as Join by Bend. | -- (Inventor Bend, Fusion Join by Bend, Creo Merge) |
| **Rolled wall** | An open sketch of lines and arcs revolved or rolled into a cylindrical or conical sheet, its arcs developed exactly as a lofted flange's are. | -- (Inventor Contour Roll, CATIA Rolled Wall) |
| **Starter shapes** | A parametric L, U, tub, hat or box as a first feature, made of base wall and flanges. | B1 1 |
| **Lofted flange** | Between two profiles (a round to a square, a cone): developed exactly where the surface is a cone, cylinder or plane, and as conical facets where it is not, with the facet count a parameter. | -- (#603) |

### 5.2 Edges, corners and cuts

| Tool | What it does | Ref |
|---|---|---|
| **Bend relief** | Automatic on every bend that ends inside a wall: rectangle, obround or tear, from the rule; overridden per feature. The flange's own, from the rule, built in M1, and its override (sec 12). | B1 5 |
| **Corner relief** | Where two or three bends meet: circle, square, tear, weld; sized absolutely or by thickness. Automatic from the rule, or a feature on picked corners. Two bends' automatic one, from the rule, built in M2 (sec 12). | B1 13 |
| **Corner closure** | Two flanges brought to meet at a corner: overlap, underlap or butt, with a gap. Built in M2 (sec 12). | -- |
| **Rip** | A gap cut along an edge of a closed solid, so it can unfold. | B1 15 |
| **Cutout** | A closed sketch cut normal to the sheet on every wall and bend it crosses. | B1 18 |
| **Wrap cutout** | A sketch cut as if drawn on the flat: through the flat, and folded back (refold, 4.1). Built in M2 as the cutout's `Wrap` (sec 12). | B1 17 |
| **Notch at a bend** | A small V or slot at each end of a bend line on the outline, for the press brake operator. | -- (#622) |
| **Hinge and perforation cuts** | Slots along a bend line so it can be bent by hand, or a laser living hinge: straight, tiled from a sketch, or wave; drawn in the flat. Built in M2 as **Bend Cuts** (sec 12). | B1 5 (perforation), B1 10 (hinge cuts) |
| **Tab and slot** | Tabs on the edge of one wall and matching slots cut through another it meets, with a clearance, so laser-cut parts locate themselves for welding. | -- (SOLIDWORKS, gk) |
| **Cross break** | Two diagonals drawn on a panel as bend lines of a small angle, shown in the flat. | -- |

Holes, pockets, fillets and chamfers of flat corners, patterns and mirrors are PartDesign's own
(3.2).

### 5.3 Convert and recognise

| Tool | What it does | Ref |
|---|---|---|
| **Convert** | A solid or shell (a box, a duct): the faces to leave open picked, the edges to rip picked, every other sharp edge becomes a bend of the rule's radius; then thickened inside or outside. | B1 4, 15, 16 |
| **Recognise** | An imported thin part (STEP) taken as sheet metal as it is: the recogniser of 4.2 way 3, its rule set read from the part (thickness, radii). Then the flat pattern and the bend table work on it. | (#95) |

### 5.4 Flat state and output

| Tool | What it does | Ref |
|---|---|---|
| **Flat pattern** | 4.7. | B1 10 |
| **Unfold / refold** | A pair: features between them are made on the flat and folded back with the part. The bends to unfold are picked; all of them by default. | -- (#296) |
| **Flat orientation** | The flat laid by a picked edge or direction, with the grain or rolling direction drawn as an arrow, kept in the DXF and the drawing. | -- (Inventor) |
| **Manufacturing properties** | Each sheet's flat size (its bounding box in the flat), area, thickness, material, bend count and largest bend angle, as read-only properties a bill of materials or a drawing table can take. | -- (SOLIDWORKS cut list, gk) |
| **Export flat** | DXF and SVG of one sheet, or of every sheet in a document in one go, one file each. | B1 10 (#462) |
| **Bend table in a drawing** | The bend table as a TechDraw table (5.6). | -- |

### 5.5 Not in this release (section 11 says why)

- One-step formability (NX); aerospace joggles and flanges on curved edges (NX, CATIA).
- Direct editing of flanges (Solid Edge synchronous).
- Bend order and bend sequence planning (Inventor, Creo, Solid Edge drafts).

### 5.6 Drawings

Every program in the survey puts sheet metal on a drawing sheet [A4]: an unfolded view with bend
axes and bend limits (CATIA [A src 48]); a flat pattern view with form annotations (Onshape
[A src 23]); bend tables placed on the draft (Solid Edge [A src 42]); a bend table annotation with
the bend order (Inventor [A src 12]); bend notes and an order table (Creo [A src 27]); PMI on the
flat (NX [A src 36]); and in SOLIDWORKS a flat pattern view with bend notes ("UP 90 R2") and the
cut list as a table (gk). In TechDraw:
- **The flat view.** A TechDraw view of the FlatPattern feature, as of any shape. Its bend lines are
  edges of their own, named after their bend, drawn as centre lines: up and down in two styles, and
  the bend's tangent lines optional, as in the DXF. The grain arrow and form outlines or centre
  marks come with it.
- **Bend notes.** A note on each bend line, its direction, angle and radius ("UP 90 R2"), attached
  to the bend by name, so a note and a dimension to a bend line survive flanges added before it
  (4.3).
- **The bend table.** Each bend's name, direction, angle, radius, allowance and value source, and
  the bend notes keyed to it by number. TechDraw has no table of its own beyond a spreadsheet view
  (`DrawViewSpreadsheet`); the bend table is a view fed by the flat pattern directly, live, and the
  manufacturing properties (5.4) can go in the same kind of view.
- **The folded views.** Bend tangent edges, between a wall and its bend, are smooth edges, so
  TechDraw's `SmoothVisible` shows or hides them as on any part; upstream's open request to hide
  them (#18251 [A5]) is that switch.
- **Live.** All of it recomputes with the part, as every TechDraw view does.

### 5.7 Audit against other programs and the addon (2026-10-30, approved)

Checked against the survey's catalogue of ten programs [A7a] and the addon's inventory and tracker
[B1][B4]. Added, all approved: extend wall, hinge and perforation cuts, starter shapes (the addon
has them); bend between walls, rolled wall, unfolding picked bends, flat orientation (most programs
have them); tab and slot, manufacturing properties (gk, not in the survey's sources); the gusset
and three more built-in forms (4.8). Bend order stays out (5.5).

## 6. The addon's documents

Users with addon parts will open them here. Without the addon installed, its objects load as
Python features with no proxy: their shapes show as last saved, and they do not recompute [B3,
#389].

**Ruled 2026-10-30 (question e): nothing is built for them.** Users keep the addon for its files;
since sheet metal lives in PartDesign, both can be installed. What follows was the proposal, kept
for the record: a **Convert addon objects** command reads each addon object's properties --
data only, no addon code runs -- and makes the native feature with the same parameters, linked to
the same inputs. The addon's classes and parameter names are in [B1]. A part whose flat pattern
changes in the conversion because the addon's table interpolation was wrong is told so. Objects
the converter does not know stay as they are.

## 7. Analysis and checks

- **Bend table readout** (4.7) with each value's source.
- **Checks on the flat:** material overlapping itself, a relief smaller than the rule's minimum, a
  flange shorter than the minimum for its radius and thickness (a rule value), a hole too close to
  a bend line. Each is listed on the flat pattern and marked in the view; none stops the recompute.
- **Thickness check** on a recognised part: where the thickness departs from the rule's.

## 8. Editing in the view

### 8.1 Working in PartDesign: tool bars that come when asked for

Ruled 2026-10-30. Sheet metal and surfaces are made in a PartDesign body, without switching
workbench. Their tool bars are part of the PartDesign workbench but are not offered, not shown and
not in the menus, until something asks for them. Showing them all the time would crowd the window,
because each of their commands makes a new object rather than adding geometry to one object. The
sketch's own tool bars are the other case: they make sense only inside a sketch's edit, which
already switches to them.

How other CAD programs keep surface and sheet metal tools out of the way (general knowledge, not
surveyed):
- **Tabs** (SOLIDWORKS, Fusion, Inventor, Creo, NX): every tool reachable, one tab shown at a time.
- **Modes entered and finished:** Fusion's Form and Flat Pattern, Inventor's Freeform and Flat
  Pattern, Creo's Style. Onshape's Sheet Metal Model stays active until finished (survey A6.4).
- **Containers that take new features:** CATIA's geometrical sets, Creo's quilts and NX's sheet
  bodies. CATIA still ties tool bars to the workbench, not to the set.

Here it is the container, with its tool bars tied to it:
- **A surface group** is a new kind of aux group in a body (`PartDesign::AuxGroup`, named
  `Surfaces`). It gathers the body's Surface features, as Sketches gathers its sketches, and the
  body's Misc group leaves them to it. Its icon is the group's folder with the Surface icon on it.
  - There is no command to make one, as there is none for the other aux groups (revised
    2026-10-30). A body keeping the auto aux groups (`Body::setAutoAuxGroup`, the body's context
    menu) makes it when the first surface comes in, and removes it with the others.
  - A body made in the GUI (`PartDesign_Body`, `PartDesign_Clone`) keeps the auto aux groups from
    the start, by the preference `Mod/Part/AutoAuxGroup`, on by default. A body made by script
    has none, so scripts and their tests see the body's group unchanged.
  - A double-click toggles it: active in the view or not. While it is active, the Surface tool bar
    is offered.
- **A solid holding sheet metal** (`PartDesign::SolidBody`) has its icon changed, by an overlay. A
  double-click toggles it active, which offers the sheet metal tool bars.
- **An edit offers them too.** Editing any Surface or sheet metal object offers its tool bars if
  they are not offered already. Leaving the edit takes them away again, but only if no surface
  group or sheet metal solid is active.

**The first sheet metal feature (ruled 2026-10-31).** The tool bars above come only with a sheet
solid active or a sheet feature in edit, so the first base wall needs a way in: a `Sheet Metal`
submenu of the Part Design menu always holds Base Wall; the full tool bar comes as above.

The mechanism is general (`Gui::ToolContext`). A module registers a context: its tool bars, the
workbenches that take them, the view's active-object key that holds them (`pdsurface`,
`pdsheetmetal`), and the kind of object it is for. The kind is what the module registers its types
with on a body (`Part::BodyBase::allowType(type, kind)`), so PartDesign tells a Surface feature
from others without linking Surface. The tool bars come back after a workbench switch, and go when
the active group is deleted or its document closed.

Built 2026-10-30 for Surface. The SolidBody half is in place, but nothing uses it until the first
sheet metal feature registers its context (M1). It is tested then.

Built 2026-10-31 for sheet metal (9bb82c8a2a), tested by `GuiSheetMetalCommands`: the Sheet Metal
tool bar (Base Wall, Flange, Flat Pattern, Export Flat to DXF) is offered while a sheet's solid body
is active (`pdsheetmetal`) or a sheet metal feature is in edit, the solid body carrying the sheet
metal overlay; the types are of the kind `SheetMetal` on a body; the Part Design menu's Sheet Metal
submenu holds Base Wall. The commands take selection and make the feature, and open its panel.
Base Wall, Flange and Flat Pattern are on the browser allowlist (76a9426063): they open no dialog,
saying a selection or a body they cannot use in the report view; Export Flat to DXF asks for a file
and is not. The panels (66d645d6c7, `GuiSheetMetalPanels`): a box for each value, of the property's
type and named after it, so the panel mirror carries it; the rule's values said; what is wrong said
in the panel after any recompute; the flange's edges listed, picked with Add, taken out with Remove.
OK commits, Cancel takes a new feature away. Found writing the manual (6324cb9e49): the labels
and choices are said in words (Material side, Outer sharp), the boxes still named after the
properties; what is read only is greyed out, and the base wall makes Length, Reversed and Midplane
read only on a plate; a value set elsewhere while the panel is open shows in its box, a box touched
only where it differs. The flat pattern's panel (a double-click on it; it is a
Part feature, its own view provider): what it lays flat, the root face picked in the view or the
first wall's, the values a bill of materials takes, OK one undo step and Cancel putting it back.
The rule's panel (a double-click on the rule; `GuiSheetMetalRulePanel`): its card, chosen from the
material list, whose first entry is the body's material (the rule following the body again); the
card said, whose it is and whether it has a gauge row for the thickness; a box for each value; for
each value the card gives (bend radius, K, minimum flange, allowance method), whether it is the
card's, the rule's own or set by hand, and a button giving it back to the card; the features that
follow the rule, and what is wrong with the first that fails. A value set there rebuilds every
feature following the rule; OK is one undo step, Cancel puts it all back.

### 8.2 In the view

- **Flanges in the view:** each flange's leg end has a drag arrow for its length and an arc handle
  for its angle, using the gizmos the Surface edge markers use (`LinearGizmo`).
- **The flat beside the folded part:** the flat pattern can be shown in the same view, laid
  beside the part, picks cross-highlighting.
- **The browser:** every panel mirrored and every command on the browser allowlist, as Surface's.

## 9. Phases and tests

| Phase | Content | Tests | Size |
|---|---|---|---|
| M0 | **Foundation and the speed gate.** The module, its `Geom/` layer and test target. The sheet model and the fold maps of a wall and a cylindrical bend. Bend allowance: K, DIN, BA and BD tables, Y. The rule object. **Measure the join** (4.4) on a 64-flange part: a glue boolean against replacing the end face directly; set the speed target from it. | gtests: a bend's map sends known points where hand computation says; every allowance method against worked examples, the addon's table case among them. A timing test for the join at N = 1, 4, 16, 64. | M |
| M1 | **A first part end to end.** Base wall (plate and profile), Flange with the full option set, extend wall, names by role, the model carried by name through a Pocket and a Hole (4.2 way 2), the flat pattern with both skins and its orientation, the manufacturing properties, the DXF. | Python: flat length of an L, U and box equals the hand sum of legs and allowances at 1e-6; a countersunk hole survives into the flat; a flange end referenced downstream survives an edit upstream; save and reopen; frozen and unfrozen. A 64-flange chain against M0's target. | L |
| M2 | **Edges, corners, cuts.** In this order (ruled 2026-11-02): hem; corner relief and corner closure; cutout; fold and jog; tab; contour flange; bend between walls; wrap cutout; hinge and perforation cuts; checks (7). The bend relief's override per feature first (built). | per tool, as M1; a wrap cutout across two bends is the same hole in the flat as cut there directly. | L |
| M3 | **Convert and recognise.** Convert a solid or shell, rip; the geometry recogniser for imported parts. | the M1 parts exported to STEP and read back give the same flat as their features do; a hand-made shell converts and flattens to the hand value. | M |
| M4 | **Flat state, the table and drawings.** Unfold / refold of picked bends, notches, the bend table panel with overrides, cross break, multi-sheet export, the drawings of 5.6. | an override changes one bend's flat and nothing else; refold of a slot cut in the flat is the slot cut folded. | M |
| M5 | **Forms, lofted flanges and the rolled wall** (moved from M2, ruled 2026-11-02: it needs walls that are not plane in the sheet model, and the lofted flange's development). User tools and the built-in set (gusset among them); starter shapes; tab and slot; the flat's three ways of showing a form; lofted flange developed exactly for cones and by facets otherwise. | a cone's development against the closed form; a form kept in the flat is the same solid moved. | M |
| M6 | **Not built** (question e). **The addon's documents** (6). | a set of addon files saved by addon 0.8.26, converted, and their flats against hand values. The files are made by the user or from the addon's own examples; this session does not run the addon (3.5). | S |

**Test rule, from the 3D sketch and Surface:** each phase's tests are written first and scored on
the code before the change, so a pass means something. Every test runs with shape values frozen
and not frozen.

## 10. Risks

1. **The join's cost** (4.4). If neither a glue boolean nor a direct face replacement is cheap on
   a large solid, a long chain stays O(N^2) in booleans. Measured first, in M0, before any feature
   depends on it.
2. **Faces in a bend that are not skins or normal cuts** (a countersink across a bend line, a
   chamfer on a bend's edge) map only approximately. They are approximated within the flat's
   tolerance and listed, not dropped as the addon's default unfolder drops them [B2].
3. **Bend position conventions.** "Material inside / outside / bend outside" is the addon's most
   reported bug (#614) [B1 5]. Each option gets a test with the expected leg length worked by hand.
4. **Recognising imported parts** is heuristic in every program. The recogniser says what it could
   not own rather than guessing.
5. **A flat that changes the user's numbers.** Correct tables give different flats from the
   addon's on some parts (3.5). The converter says so per part.
6. **Upstream.** If upstream later moves the addon into core (its PR #31913 was closed for an FEP
   first [A5]), it will sit at `src/Mod/SheetMetal`, which is one more reason for our module to
   have another name (question a).

## 11. Not proposed

- **Porting the addon.** Its structure is the slowness and the one-skin flat [B2]; its parameters
  and its tracker are what we take.
- **A sheet-first model replacing the solid** (4.1's alternative): every PartDesign tool would need
  a sheet metal twin.
- **Bundling the addon.** Ruled out on 2026-10-26: native C++.
- **A worker process for the flat.** The flat builder is kept pure so it can move; the worker is
  docs/ComputeBoundaries.md's work.
- Section 5.5's list.

## 12. Questions

**Progress:**
- M0's speed gate, the join, measured (sec 4.4, 7ef92a8d41).
- M0's geometry layer: `PartDesign::SheetMetal`, the bend allowance by every method of sec 4.6,
  and the cylindrical bend's fold map, with gtests (`PartDesign_tests_run`, 13) (41d487593a).
- The tool bars of sec 8.1, built for Surface (c10f1f68e8 and the commits before it); the surface
  group made by the body, not by a command, and the auto aux groups on in new bodies (revised
  2026-10-30).
- The Material module's `SheetMetal` model (sec 4.6): a gauge table, the allowance method and a
  bend table, with a gtest (2026-10-30).
- M0's rule object, `PartDesign::SheetMetalRule` (4.6, ruled 2026-10-31), reading that model,
  with `TestSheetMetalRule` (9) and a gtest for the card's bend table (2026-10-31). Its tree
  place under the solid body, and the solid body a lone sheet needs, come with the first
  feature that links it (M1).
- The sheet model (4.1), `SheetMetal::Model`: walls as slabs, bends as `CylBend`s, the owner of
  a point or a face by geometry (bends tried first, a wall bounded by the side of each bend it
  meets), and the flat of every element from a root wall, either way across a bend; gtests
  `SheetMetalModel` (4) (75f4438372).
- M1's base wall, `PartDesign::SheetMetalBaseWall` (5.1), with `TestSheetMetalBaseWall` (12)
  (3b4d08ea6a). As built:
  - A closed sketch makes a plate; an open sketch of lines a profile wall, a bend of the rule's
    radius at each corner, extruded by `Length` (`Reversed`, `Midplane`). Lines in a row along
    one direction are taken as one; an arc in the profile, a turn back on itself and a leg too
    short for its bends are refused.
  - `MaterialSide`: `Outside` puts the sketch on the first bend's inner skin, `Inside` on its
    outer skin, `Middle` across; the side holds along the profile, so a bend turning the other
    way has the sketch on its outer skin. For a plate: the side of the sketch's normal, the
    other, or across.
  - Each wall and bend is a piece of its own, its edges named by role (`Wall1Skin1`,
    `Wall1Start`, `Bend1Inner`, `Bend1Outer`), joined in one fuse; so the caps stay split at
    the bend lines and every face lies in one element. `Refine` is off and hidden, as it would
    merge faces of two elements.
  - Every sheet metal feature has `PartDesign::SheetMetalExtension`: `Rule`, a global link,
    and `SheetModel`, the model saved with the file (read only from Python), with
    `sheetElementOf(subname)`. A feature put in a body takes the rule of the sheet feature it
    builds on, or the first makes one; the body's `AddSolidBody` is turned on. A bend's K in
    the model is the one that gives the rule's allowance, whatever the method.
- The rule's tree place (4.6) (a164da828d): a body's only solid gets a SolidBody when it holds
  sheet metal; the SolidBody's view provider claims the rules its features follow, those in its
  body, before the features; Misc leaves rules to them. The body's export list left the rule at
  its top level, since it counts only what a child links locally and `Rule` is global: the body
  leaves out what its solid bodies claim, through a hook on the group extension
  (`excludeFromExport()`). Tested by `GuiSheetMetalTree`.
- M1's flange, `PartDesign::SheetMetalFlange` (5.1), with `TestSheetMetalFlange` (13)
  (65b9cac04f). As built:
  - On edges where a wall's skin meets its end (an end face across the sheet), all joined in one
    boolean; the edges track the feature built on, as a dress-up's `Base` does.
  - It bends toward the side of the skin the edge is on, `Reversed` toward the other, through
    `Angle` with the rule's radius.
  - `LengthType`: `Leg` the straight part, `InnerSharp` and `OuterSharp` from where the inner or
    outer skins of wall and leg meet (default `OuterSharp`).
  - `BendPosition`: `MaterialInside` puts the leg's outer skin at the wall's end,
    `MaterialOutside` its inner skin, both seen at the virtual sharp for an angle not 90 (a cut
    back of (R + T) tan(A / 2) and R tan(A / 2)); `BendOutside` starts the bend there. `Offset`
    moves the bend out, the wall carried on to it. Where the bend starts inside the wall, the
    wall is cut back under the flange, one cut before the join.
  - `Gap1`, `Gap2` narrow the flange at the edge's ends; the wall is cut back under it only.
  - The bend relief (5.2, brought into M1 on 2026-11-01): a flange narrower than its edge, by
    gaps or by a profile, leaves the wall beside it meeting the bend face to face across the
    flange's side, and the flat did not close. So at each end where the flange stops inside the
    wall's end, and the bend starts inside the wall (not `BendOutside`, nor carried out by
    `Offset`), a relief is cut into the wall beside the bend, automatically, from the rule:
    `ReliefWidth` along the edge, from the wall's end to `ReliefDepth` past where the bend
    starts, through the sheet. `ReliefShape` `Rectangle`, or `Obround` (its deep end a half
    circle of its width, the tip at the same depth), or `Tear` (ruled 2026-11-02: a slit the
    rule's `Gap` wide, from the wall's end to where the bend starts, so its flat closes too;
    no gap, no slit). The relief's faces are named after the bend (`Bend1Relief1`,
    `Bend1Relief2`). In the model the bend stops at its sides there (two limit planes), as its
    tube runs on through the wall beside it and owned the relief's faces. Tested by
    `testRelief*` (7). The relief where a bend meets another wall than its own is M2's.
  - The relief overridden per flange (5.2, M2, 2026-11-02): the flange's `ReliefShape` is
    `Rule` by default, following the rule, its `ReliefWidth` and `ReliefDepth` read only; any
    other shape is the flange's own, its width and depth set from the rule's when chosen and no
    longer following them. A tear is the rule's `Gap` wide either way. In the panel. Tested by
    `testReliefOverride*` (4) and the panels' GUI check.
  - Added (9e98a08d8a): `Tangent` measures from the plane across the leg that touches the outside
    of the bend (the straight part L - (R + T) from 90 deg on, L - (R + T) sin A below); negative
    gaps carry the flange past the edge's ends; `ParallelTo` gives the angle from a plane face the
    leg runs parallel to, `UpToFace` the length from one it runs up to.
  - The miter (96e7a8248c): two edges of one flange sharing a corner, bent the same way, each
    side (bend, leg, any wall carried on) cut by the plane through the corner halving it, half
    the rule's `Gap` off it each way; exact for two sides alike, any two kept apart. The model's
    elements gained limit planes (`Element::limits`), the side of each an element stops at, as
    the two sides' slabs and tubes overlap at the corner and a face there was owned by whichever
    came first; saved as an attribute only where there are any. The flat: a bend cut at a slant
    does not unroll into a plane, so its cut face is ruled between the two skins' unrolled
    edges; unrolling keeps a bend's volume only where it is cut square (0.19 mm^3 a corner at
    R = T = 1). Tested by `testMiter`, `testMiterTray`, `testNoMiterApart`.
  - Mitered against an earlier flange (ruled 2026-11-02): an end of the edge that stops where an
    earlier feature's bend on the same wall starts, that bend's material just beyond it, and
    with no gap asked for there, meets that flange at a corner (a hem's fold, at 180 deg and
    past, is not one). The flange is carried on past the corner, the wall cut back under its
    edge only, and the two are mitered at the plane mirroring the earlier leg's outer skin onto
    this one's, which for two flanges alike is the plane halving the corner: this flange's
    pieces stop on their side, the earlier flange's material in front of where its bend starts
    is cut from the part on the other, the rule's `Gap` between, the bend's and the leg's in two
    cuts so the face each leaves is its own element's (one cut left one face across both, which
    the flat could not lay out). The model gets the limits as one flange's miter does. The part
    and its flat are then one flange's on both edges, the test's oracle: `testMiterAgainstEarlier`
    (both orders), `testMiterAgainstEarlierTray` (four flanges round the plate),
    `testNoMiterAgainstEarlierWithGap`.
  - The leg's shape (ruled 2026-11-01: the sketch in the leg's plane, as SOLIDWORKS edits a
    flange's profile; a sketch on the wall drawn as in the flat and wrapped through the bend,
    which would allow a bend that is not a rectangle, is for later): `Profile`, a closed sketch
    lying parallel to the leg -- anywhere, it is taken onto the leg's outer skin -- whose
    outline stands on the outer virtual sharp, so the sketch's height is the length as
    `OuterSharp` measures it. From the sharp to where the leg starts, (R + T) tan(A / 2) up the
    outer skin, the outline is a rectangle, and its span along the edge is the bend's width (the
    flange's gaps); above it the outline is the leg, holes and all, as wide as it likes. On one
    edge only, and not with `UpToFace`; `Length`, `LengthType`, `Gap1` and `Gap2` are read only
    while it is set. Refused: a sketch not parallel to the leg, open, not standing on the sharp
    or reaching past it, narrower or holed within the bend, or with nothing above it. The panel
    offers the body's sketches that are not built on the flange. Tested by `testProfile*` (7)
    and the panel's GUI check.
- M1's extend wall, `PartDesign::SheetMetalExtendWall` (5.1), with `TestSheetMetalExtendWall` (7)
  (a229435ab6), its command and panel (234138ce56): each face a wall's end across the sheet,
  carried along its normal by `Length` or up to `UpToFace`'s plane; a negative length cuts the
  wall back, one that leaves nothing is refused. The extension is the wall's own, so the model is
  as it was and the flat grows with it. The fuse left a seam where the end was, across both skins
  and caps (faa05f8c91): those seams alone are unified, every other edge kept, as a plain refine
  would merge the caps across the bend lines. The gaps and the clearance (as the addon's,
  B1 7): `Gap1` and `Gap2` narrow the extension in from the end's two ends, the first and second
  along the end face crossed with the wall's normal; gaps that leave nothing are refused. An
  extension that would meet another wall of the part, run into it or end on it, stops the rule's
  `Gap` short of it: the prism carried one gap further is put against the part, and the nearest
  point of what they share sets the length; one with no room is refused. Both by length and up
  to a face. Tested by `testGaps`, `testClearance`; the panel has the gaps.
- M1's flat pattern, `PartDesign::SheetMetalFlatPattern` (4.7), with `TestSheetMetalFlatPattern`
  (10) (ad6b48f059). As built:
  - `SheetMetal::flatten()` in the geometry layer, a pure function: each face of the folded
    solid through its element's map (a wall's rigidly; a bend's edges through the fold map, a
    line where an edge maps to one, else a B-spline through samples, the face rebuilt on its
    plane), sewn into one solid with the root wall's skin on XY, the material above.
  - `Source` is the folded part, a sheet metal feature or one after it; its model is the nearest
    sheet metal feature's, so a pocket's faces are placed by where they lie (4.2 way 2, tested).
    `RootFace` is a wall's skin, the first wall's first skin if none.
  - The shape is a compound of the flat solid and one bend line per bend, along the bend's
    middle on the inner skin, named after the bend.
  - Refused by name for now: a face no element owns, a face of a bend that does not lie flat as a
    plane (risk 2's approximation is still to come), a flat that does not close.
  - The manufacturing properties of 5.4 (e827522697), read-only in a `Manufacturing` group:
    `FlatLength`, `FlatWidth` (its size on XY, the longer first), `FlatArea` (the blank's),
    `Thickness`, `MaterialName` (the rule's card), `BendCount`, `MaxBendAngle`, and
    `BendDirections`, each bend `Up` where the material comes up toward one looking down on the
    flat (its inner skin on top).
  - The DXF (e827522697), `SheetMetal::writeDXF()` in the geometry layer, `exportDXF(path)` on
    the flat pattern: R12 from the flat's face on XY with the faces of one plane merged, the
    outline on `OUTLINE`, inner contours on `INNER`, bend lines on `BEND_UP` and `BEND_DOWN`,
    each labelled on `LABELS`; lines, arcs and circles as they are, other curves as polylines.
  - The flat's faces are named after the folded faces they lay out (`flatten()` returns each
    folded face with the face of the solid it became, followed through the sewing and the
    solid's fix; the feature names by that history, op code `SMF`), and its edges and vertices
    after the faces they bound; so a reference to the flat follows the part through an edit.
    Tested by `testFacesNamedAfterFolded`, `testFaceReferenceFollows`.
  - The bend tangent lines, asked for by `TangentLines` (off by default): each bend's two lines
    where it meets its walls, on the bend line's skin, the allowance apart; a third compound in
    the shape, named `Bend1Tangent1`, `Bend1Tangent2`, and the layer `BEND_TANGENT` in the DXF.
    Tested by `testTangentLines`.
  - The flat's orientation: `AlignEdge`, an edge of the folded part, turns the flat about Z so
    that the edge's image runs along X (from its first parameter to its last); one that does not
    lie straight along the flat (a hole's circle) is refused by name. `Grain` (off by default)
    draws the grain or rolling direction, `GrainAngle` from the flat's X, as an arrow of three
    lines on the flat's top through its middle, named `Grain`, `GrainHead1`, `GrainHead2`, and on
    the layer `GRAIN` in the DXF. Tested by `testAlignEdge`, `testAlignEdgeNotStraight`,
    `testGrain`.
  - The SVG (`SheetMetal::writeSVG()`, `exportSVG(path)`; the command Export Flat... offers DXF
    or SVG): the DXF's layers as groups of the same names, Inkscape layers, one with nothing on it
    left out; as large as the flat, in mm, looking down on it with Y up; lines, circles and arcs as
    they are, other curves as polylines. The faces on XY that both draw come from one function,
    `facesOnXY()`. Tested by `testSVG`, `testSVGHole`, `testSVGArcs` (the last written when a
    rendering showed a slot's half circles bulging into it: turning Y to run down turns SVG's
    sweep too). Qt's own SVG renderer drops text of a small user-unit size at some scales, so a
    label can be missing in a Qt view of the file; Chrome and Inkscape draw it.
- M2's hem, `PartDesign::SheetMetalHem` (5.1, ruled 2026-11-02), with `TestSheetMetalHem`
  (16), its command, icon and panel. As built:
  - On edges where a wall's skin meets its end, as the flange's (the edge read by
    `SheetMetal::wallEdge()`, which the flange now uses too); toward the side of that skin,
    `Reversed` toward the other.
  - `HemType` `Closed`: a 180 deg bend whose inner skins are the rule's `Gap` apart, an inner
    radius of half that; `Open`: the same with the hem's own `Gap`, read only while closed.
    `Length` runs from the fold's outer face to the leg's end; one no longer than the fold
    leaves no leg and is refused.
  - `Position` `Inside` (the default) cuts the wall back R + T so the fold's outer face is where
    the wall ended; `Outside` adds the fold past the end.
  - `Teardrop`: a bend of the hem's own `Radius` through `Angle`, past 180 deg up to 270, its
    leg heading back toward the skin and stopping `Gap` short of it (its corner nearest the
    skin starts R (1 - cos A) over it and comes down at -sin A a length), so `Length` is read
    only; one whose bend comes within its gap of the skin has no leg and is refused. `Rolled`:
    the bend alone, from 90 deg up to where its end would come back within the rule's `Gap` of
    the skin; `Length` and `Gap` read only. Inside cuts both back R + T, their reach past the
    wall's end.
  - In the model, a bend and a wall (the leg) as a flange's; a teardrop's leg and a roll's end
    (a wall with no material) stop at a limit plane where the leg ends, as their slabs would
    run on through the wall they head back to. The flat lays the hem out with K's allowance at
    its radius.
  - A bend past 180 deg: which side of a bend a point is on was its angle about the axis taken
    from -90 deg, where the wall before the bend runs off to; a teardrop's leg runs through
    there, and was taken as before the bend, owned by nothing. Each wall now takes the angles
    from where it runs off to (`CylBend::side()`, `sideAfter()`), and the bend's own material
    from halfway round the turn it leaves out, so a roll past 270 deg is its own; nothing
    changes for a bend up to 180 deg. gtests `SheetMetalModel.pastHalfTurn`,
    `SheetMetalFold.roll`.
  - `Gap1`, `Gap2` narrow the hem at the edge's ends, as a flange's; where it stops inside the
    wall's end with the wall cut back (`Inside`), the rule's bend relief is cut beside the fold
    and the fold stops at its sides in the model, as a flange's bend does. The relief's cuts are
    `SheetMetal::reliefCuts()`, which the flange now uses too. The relief overridden per hem
    is not built.
  - Two edges of one hem meeting at a corner are refused, as their folds would run into each
    other.
- M2's corner closure, `PartDesign::SheetMetalCornerClosure` (5.2, ruled 2026-11-02), with
  `TestSheetMetalCornerClosure` (6), its command, icon and panel. As built:
  - `Faces`: the end of a flange's leg on each side of the corner, the first then the second,
    both flanges bent from the same wall; anything else is refused by name. The ends may be
    square (a corner the gaps left open) or slanted (one a miter made).
  - Each side is made again from far enough back that what a miter took comes back (behind
    where the other side's bend starts, a thickness more): its bend's and its leg's sections,
    from the model, carried on past the corner; what was there, and the wall at the corner in
    front of where each bend starts, is cut from the part.
  - `CornerType` `Butt` (the default) stops both at the plane mirroring one leg's outer skin
    onto the other's, the rule's `Gap` between, as the automatic miter; `Overlap` carries the
    first side's bend and leg on to the second leg's outer skin, the second leg stopping the gap
    short of the first's inner skin and its bend the gap short of where the first's bend starts,
    so the bends never meet; `Underlap` the other way round. In the model each side's limits
    toward the corner (a miter's, a gap's) give way to the new ones.
  - A gap's relief at the corner stays where it reaches below where the bends start (1 x 1 each
    with the rule's defaults); filling it is not built. Oracle: `Butt` on an open corner is the
    automatic miter's part less those two, with the same flat.
- M2's corner relief (5.2, ruled 2026-11-02: drawn in the flat, folded back), at every miter -- one
  flange's, a flange's against an earlier one, a corner closure's `Butt` -- from the rule:
  `CornerReliefShape` `Circle` (the default) of `CornerReliefSize` across, or `Square` of that
  side along the bends' axes, centred where the two bends' lines (each strip's middle) cross in
  the flat; `None` and `Tear` cut nothing more than the miter's gap. As built:
  - `SheetMetal::cornerReliefFace()` draws the shape on the root wall's first skin, in the frame
    `Model::toFlat()` maps to; `SheetMetal::refold()` takes it back: for a wall rigidly, through
    its thickness; for a bend the part in its strip onto its cylinder, exactly -- the flat's
    (Y, Z) to the cylinder's (u, v) is affine, so each outline curve, made a B-spline, keeps its
    knots and weights and has its poles mapped -- capped on two cylinders a little inside and
    outside the sheet, ruled between them (radial lines). Each solid is trimmed to its element
    by its limits and the side of each bend it meets, and all are cut from the part.
  - A plane's point may lie anywhere on it -- a miter against an earlier flange keeps one 34 mm
    from the corner, a carried-on bend's frame sits far down its axis -- so every box standing
    for a half-space or a strip is placed about the shape, taken onto the plane: placed about
    the point, the corner was out of reach and went uncut.
  - `flatten()` lays out a bend's face across the thickness that the relief has cut as well as a
    miter (neither plane nor ruled) as a surface filled to its outline, the edges its
    neighbours share as they are. The flat with none overlaps itself a little at a mitered
    corner (a bend cut at a slant unrolls its skins differently, sec 12 above), where booleans of
    its solid fail; the relief cuts that away.
  - Tested by `testCornerRelief*` (4): on each skin of the flat nothing is left inside the shape
    and nothing changed outside it, within 1e-5 mm^2 (the outline in the flat is a B-spline
    through samples of its image), face by face in the plane.
- M2's cutout, `PartDesign::SheetMetalCutout` (5.2, ruled 2026-11-02), with
  `TestSheetMetalCutout` (9), its command, icon and panel. As built
  (`SheetMetal::cutoutTools()`):
  - A closed sketch (`Profile`, a `ProfileBased`'s), cut against its normal as a pocket's,
    `Reversed` along it. Its prism runs on from the sketch: what lies behind the sketch is not in
    its way, so a sketch drawn on the plate's top across a bend line leaves the bend, which rises
    behind it, whole; a skin in the sketch's own plane is taken by a prism from just behind it.
  - The skins facing the sketch: a wall's whose normal out of the material runs against the
    way (one edge on, a leg standing under a sketch above the plate, is not cut), and of a bend's
    cylinders the part where the normal out of the material does, the cylinder split by the
    plane through its axis across the way where it turns side on. What the prism covers on each
    is a piece; `UpToFirst` takes from each piece what the others' prisms along the way cover,
    so only what the sketch meets first is left.
  - Each piece is laid in its element's flat, a wall's as it is, a bend's exactly (the
    cylinder's (u, v) to the flat's (Y, Z) is affine, so each pcurve, made a B-spline, has its
    poles mapped), and folded back by `refold()`: through the sheet, trimmed to its element. The
    hole's sides are normal to the sheet everywhere, a bend's along its radius. `refold()` now
    keeps a hole in a face it folds back onto a bend.
  - The model is the one before; the flat lays the hole out as any other face. Refused by name:
    an open sketch, one that meets no skin facing it.
- M2's fold, `PartDesign::SheetMetalFold` (5.1, ruled 2026-11-02), with `TestSheetMetalFold`
  (11), its command, icon and panel. As built:
  - `Profile`, a sketch of a line (collinear pieces taken as one) parallel to a wall, taken onto
    the wall's inner skin and across it whole; the nearest such wall. It bends through `Angle`
    with the rule's radius toward the sketch's normal (`Reversed` the other way).
  - `LinePosition`: the strip the bend takes in the flat, the allowance wide, about the line
    (`Middle`, the default), from it on the moving side (`Start`), or up to it (`End`).
  - The side that moves: the one `Moving`'s face is on, else the smaller by volume. The part is
    split at the line through the wall's thickness; each side's faces tell the elements on it,
    and an element on both (a flange's bend along an edge the line crosses) is refused by name,
    as is any but the wall in the strip.
  - The strip is cut from the wall; the bend is its section swept along the axis over the
    wall's span there, and what the strip lacks of the wall -- a hole, a notch -- is taken from
    it along its radius (`SheetMetal::bendSolid()`, the face of the bend's flat taken onto its
    cylinder, now refold()'s too). The moving side is carried by the bend's `refold()` and the
    three fused, so the flat is the part as it was, holes and flanges on the moving side with it.
  - In the model, the wall's moving side is a wall of its own after the new bend, its limits
    with it; every element on the moving side is carried the same way, a bend on the wall moved
    over to the new one.
- M2's jog, `PartDesign::SheetMetalJog` (5.1, ruled 2026-11-02), with `TestSheetMetalJog` (13),
  its command, icon and panel. As built, a fold (it derives from `SheetMetalFold`, whose
  execute() takes an optional run and second bend):
  - The line, the wall, the side that moves and what is refused as the fold's; the line is the
    first bend's (`LinePosition`). `Angle` is 0 to 90 deg, 90 by default.
  - The step s, a skin to the same skin, is `Offset` (`Step`), `Offset` + T (`Inside`) or
    `Offset` - T (`Overall`). The run between the bends is (s - (2R + T)(1 - cos A)) / sin A;
    less than none is refused, giving the least offset in the measure chosen. The jog is
    2 BA + run in the flat and (2R + T) sin A + run cos A across.
  - The wall is taken from the strip's start for the jog's width across (`FixProjectedLength`,
    the default) or for its length in the flat. The first bend is the fold's; the run the
    wall's skin there made solid and carried by the first bend; the second bend is laid in the
    flat after the run on the far skin, turning back, and carried by the first. The moving side
    goes through both bends' `refold()`, pushed out first by what the jog adds (none when it is
    taken from the wall), so with the length fixed it is only lifted.
  - What the strip lacks of the wall (a hole, a notch) is taken from each bend along its
    radius and from the run straight through, as the fold's; past the width taken, a jog that
    adds material carries the wall's section at that line on, so a hole across it is stretched
    into a slot by what the jog adds (`testHoleAcrossEnd`), and the missing faces are one cut
    from one rectangle, the hole and its stretch one face.
  - In the model: the run a wall after the first bend, the moving side a wall after the second.
- M2's tab, `PartDesign::SheetMetalTab` (5.1), with `TestSheetMetalTab` (9), its command, icon
  and panel. As built:
  - `Profile`, a closed sketch (holes in it kept), on a wall's skin or a plane parallel to it:
    of the walls parallel to it whose first skin the sketch, taken onto it, meets (touching an
    edge is enough), the one whose slab it lies nearest. Made solid through that slab and
    fused, so the part keeps one solid; a tab drawn over the wall adds only what lies off it.
  - A wall is a slab bounded by the side of each bend it meets and its limits, so a tab inside
    that is the wall's already: the model is the one before, and the flat lays it out with the
    wall. Sampled at its vertices, its edges' middles and its faces' centres, a tab reaching
    past a bend of the wall is refused, naming what it runs into; one meeting no wall, and an
    open sketch, are refused too.
  - The fuse leaves the wall's skin split along the tab's outline and the wall's old edge; those
    seams are merged (UnifySameDomain keeping every other edge: an edge goes only where its two
    faces lie in one plane and it lies on the tab), so the wall's skin is one face each side, on
    the part and in the flat (`testOneSkin`, `testOneSkinOnFlange`). The feature's own Refine
    stays off, as it would merge faces of two elements. A merge that does not keep a valid
    solid of the same volume is dropped and the seams stay.
- The model, a wall after a bend: it owned every point of its slab not before the bend by angle
  about the axis, so a point far behind the bend, in the bend's angle but nowhere near its
  radius, was the wall's -- a flange's leg on a plate's far end, as high as a jog's stepped side,
  had its faces owned by that side, and its flat did not close. It now also stops at the line
  where the bend ends, on the side it runs off to (`behindEnd()` in `Model::contains()`). The
  wall before a bend keeps the angle alone: the wall beside a bend that ends inside its end runs
  on past the bend's start (`TestSheetMetalJog.testFlangeBehind`).
- The model saved with a bend from a later wall: a fold or a jog carries what is on its moving
  side and starts those bends from its own wall, made after them. Reading the model back added
  each bend as it came and refused a wall not read yet, so the whole model was lost on opening
  and a flat, or any sheet metal feature after, failed. The bends' walls are now checked once
  all are read (`PropertySheetModel::Restore()`; `testSaveAndRestore` in the fold's and the
  jog's tests).
- M2's contour flange, `PartDesign::SheetMetalContourFlange` (5.1, ruled 2026-11-02), with
  `TestSheetMetalContourFlange` (12), its command, icon and panel. As built:
  - A subclass of the flange (`contour()`), its angle, length, bend position, offset, parallel
    and up-to faces and `Reversed` hidden; `Profile` is the open sketch, `Edges`, the gaps and
    the relief as the flange's.
  - The sketch is one open wire of lines (an arc is refused, a closed one too). Its start is
    the end of it on one of the edges (either end); every point of it lies on the plane through
    the start square to that edge. Its points are kept as their way along that edge's end
    normal and skin normal, and laid so on every edge from where the gaps leave it.
  - The base wall's profile builder is shared (`SheetMetal/Profile`, `ProfileBuilder`): the
    chain is the wall's skin through the edge, then the sketch's points, its first line the
    wall itself (`first`), the material from -T to 0 along the skin's normal. It makes a wall
    and a section on each line after, a bend at each corner; where the wall's straight part
    ends sets how far the wall is cut back under the flange (with the flange's relief at a
    gap), or carried on when the first line goes straight on from the wall. The way the first
    turn goes is the flange's `up`.
  - On several edges the flange's miters cut every piece of each side and limit every element
    it made; the corner relief goes between the first bends. An edge meeting an earlier
    flange's bend at a corner is refused (the flange's miter against an earlier flange is for
    its own leg only): give it a gap there.
  - A chain turning back into the sheet is refused: the join must hold the part and the
    flange's pieces whole (their volumes add up).
- Open: a flange whose edge stops where a jog's bend starts treats that bend as an earlier
  flange's at a corner and is mitered against the jog's run; its flat does not close. Seen
  while building the contour flange; not chased.
- M2's bend between walls, `PartDesign::SheetMetalJoin` (5.1, ruled 2026-11-03), with
  `TestSheetMetalJoin` (10), its command (**Join by Bend**), icon and panel. As built:
  - Two sheets come from two base walls in one body: each base wall adds its walls to the one
    model, so a body may hold walls no bend joins yet, and its flat fails until one does
    (`Model::placement()` says the element is not joined to the root).
  - `Edges` is two edges of the base, each a flange's (`wallEdge()`): the first's wall is the
    bend's before, the second's its after. The way out of the first wall is its end's normal,
    the way into the second against its end's; each wall's inner skin is the one facing the
    turn's inside, so either skin's edge picks the same end. The bend's centre lies R in from
    both inner skins, its angle the turn's; each wall is extended (a wall section) or cut back
    to the line its inner skin meets the bend on, over the length the two edges share along
    the bend. A wall cut back and running on past that length gets the flange's relief beside
    the bend (`ReliefShape`, the rule's or the join's own), and the bend is limited at its
    ends there.
  - Refused: not two edges; both on one wall; walls already joined (`placement()` reaches one
    from the other: a loop does not lie flat without a rip); edges not parallel; parallel
    walls; edges sharing no length; a cut-back leaving nothing of the wall where the bend
    starts; the cut-back parting the part into more solids; the bend running into the sheet
    (the join's volume is not the part's and the pieces' added).
  - The model: a bend stopping inside its wall's end bounded that wall by angle over all its
    length, so the part of a wall running on beside the bend, past the angle the bend turns
    from its start (the second wall's, cut back under the bend), was nobody's. A bend now
    bounds its walls only between its limits along its axis (`besideBend()` in
    `SheetMetal/Model.cpp`); `testWiderTrimmed` failed without it.
- Open: a wall's slab has no extent across it in the model, so with two sheets in one body a
  face of one lying in the plane of the other's skin can be taken for that wall's (an end face
  at x 50 beside a wall whose skin is x 50 was, and `wallEdge()` refused the edge). Seen while
  building the join; the tests keep clear of it.
- M2's wrap cutout, the cutout's `Wrap` (5.2, ruled 2026-11-03), with seven more cases in
  `TestSheetMetalCutout` (16) and its box in the cutout's panel. As built
  (`SheetMetal::wrapCutoutTools()`):
  - The wall that stays is the one whose frame lies parallel to the sketch and whose skins
    (its planar faces parallel to it) lie nearest the sketch's faces; a tie goes to the wall
    added first. The sketch is moved straight onto that wall's first skin; the side it faces
    does not matter.
  - The flat is laid with that wall fixed (`flatten()`, its first skin on XY), and the sketch
    with it. Each element's faces lying on XY there are its part of the blank: what of the
    sketch lies on them (a common in the plane, fuzzy to 1e-6) is folded back by `refold()`
    through that element only. What lies off the blank -- past its outline, in the gap at a
    corner, in a hole already cut -- cuts nothing, though the element's slab runs on through it
    folded (`testWrapCornerGap` holds that).
  - `Reversed` and `UpToFirst` are read-only while `Wrap` is on. Refused by name: a sketch
    parallel to no wall, one lying on none of the flat.
- M2's hinge and perforation cuts, `PartDesign::SheetMetalBendCuts` (5.2, ruled 2026-11-03),
  with `TestSheetMetalBendCuts` (8), its command (**Bend Cuts**), icon and panel. As built
  (`SheetMetal::bendCutTools()`):
  - `Bends` is a face of each bend to cut, of the base; the faces' owners are the bends, a bend
    picked twice cut once. The bend's extent along its axis is that of its faces in the part.
  - Laid out in the bend's own flat (across the strip from 0 to its developed length, along the
    axis), each piece moved into the flat with the wall before the bend fixed and folded back by
    `refold()` through that bend only. `Slots` (the default): as many as fit between the
    `EndMargin`s with a `BridgeLength` between, centred, each `SlotLength` long; `SlotWidth` 0
    the whole strip, else that much about its middle. `Staggered`: `Rows` rows evenly across,
    `SlotWidth` each (0: half of a row's share), every other row shifted half a slot and a
    bridge, its end slots cut short by the margins. `Sketch`: the `Tile` sketch's faces in its
    own frame, X along the bend and Y across from the strip's middle, placed as slots as long as
    their extent along X.
  - The model is the one before. Refused by name: a face not a bend's, a pattern fitting no
    whole slot within the margins, a sketch pattern with no tile or no closed shape.
  - The panel gained a box for an integer (`Rows`); `Tile` is a sketch link, chosen as a
    cutout's profile is.
  - Found while testing: a tile measured with a box padded by the shape's tolerance (1e-7 a side)
    did not fit where it just fits, and one was dropped; the tile's extent is taken exactly now
    (`testSketchFits`). A tiled circle's flat was 0.015 mm^2 off: the flat's fault, below.
- A bend radius of a feature's own (ruled 2026-11-03), on the flange (and so the contour
  flange), the join by bend, the fold (and so the jog) and the base wall's profile: `UseRuleRadius`
  and `BendRadius`, in their panels; `SheetMetalExtension::bendRadius()` gives the one a feature
  bends at, `radiusSwitched()` greys `BendRadius` while the rule's is used and starts it at the
  rule's when turned off. Each feature took the rule's radius in one place; K follows the radius
  given (`modelK()`), and every length worked from R (the cut back, the leg, the sharps) with it.
  Refused: a radius not more than 0. `testOwnRadius` (flange, join, fold) and
  `testProfileOwnRadius` (base wall).
- The flat as a view (ruled 2026-11-03), its first step: `PartDesign::layFlat()`
  (`SheetMetalFlat.h`) lays any feature of a sheet flat -- a sheet metal feature, or one after
  them by the sheet's model -- by `FlatSettings`, with what a bill of materials takes; what the
  flat pattern did, without the feature. The sheet's solid (`SolidBody`, which a sheet always
  has) holds the settings (`FlatRoot` by the wall's name, `FlatSecondSkin`, `FlatAlignEdge` by
  element name, `TangentLines`, `Grain`, `GrainAngle`) and the tip's values, read only and
  hidden on a solid of no sheet metal (`FlatLength` ... `BendDirections`, and `FlatStatus`, why
  the tip does not lie flat). `SolidBody::flatOf()` keeps a feature's flat until its shape or
  the settings change. The sub-object `Flat` of every feature of the solid, and of the solid
  (its tip's), is that flat, an element of it after the dot (`Flange.Flat.Face3`).
  - Part keeps a sub-object's shape until the object itself changes; a feature's Flat depends on
    its solid's settings, so the solid clears its features' (`PropertyShapeCache::clearShapes()`,
    new) when one changes.
  - The solid lays its tip flat on each recompute, for the values: the sheet metal suites take
    about 40% longer (the flange's 10 s to 14 s).
  - Open: a flange whose end runs into a bend of the wall it is on (on the top edge of an L's
    leg, up to the L's bend) is valid folded, but does not lie flat ("faces overlap or are
    missing"); the flat pattern the same. Found writing `testSolidFlat`.
- The flat, found by the bend cuts: a hole cut across a bend lay flat 2.7e-5 mm^2 short of the
  shape drawn (its outline a B-spline through 24 samples of its image), and its wall, ruled
  between the outlines on the two skins, each sampled on its own parameters, leaned up to
  0.25 mm across the hole, so the flat's volume, and `FlatArea` with it, was off (0.015 mm^2 for
  eleven circles). Now (`SheetMetal/Flat.cpp`): an edge on a bend's cylinder is mapped exactly,
  its pcurve's poles sent through the unrolling, which is affine in the cylinder's (u, v); each
  edge is mapped once, the bends' skins first, so the faces sharing it meet on it; and a face
  across the sheet whose edge on one skin lies over its edge on the other (within 1e-4, the
  boolean leaving them 4e-6 apart) is upright, on the extrusion of the one, its edges' tolerance
  taking the rest. A miter's slanted face is ruled as before. `testWrapCircleOnBend` holds a
  circle on a bend to pi of the blank at 1e-6, its wall square to 1e-6; it was 4.8e-5 off.

**Ruled 2026-11-02, M2:**
- The order of M2's tools as sec 9 has it; the rolled wall moves to M5.
- The hem: four types, closed (flat), open, teardrop and rolled; no double hem. It sits
  `Inside`, the fold's outer face where the wall ended and the wall cut back so the part keeps its
  size (the default), or `Outside`, added past the wall's end.
- A closed hem's skins never touch, as one solid would not stay valid: it is a 180 deg bend
  whose inner skins are the rule's `Gap` apart, an inner radius of half that, an ordinary bend
  for ownership and the flat. Its allowance is K's at that radius unless the material card's
  bend table has a 180 deg row, which wins. An open hem is the same with a gap of its own; a
  teardrop bends past 180 deg and its leg closes back to the wall the gap short of it; a rolled
  hem is one bend of an angle and a radius, with no leg.
- A fold on a sketched line moves the side of the part picked, and what is already on that side
  (holes, pockets) goes with it, through each element's fold map.

**Ruled 2026-11-02, M2's corners** (Butt's meaning and the relief's cut the same day):
- Two flanges of separate features meeting at a corner: the later one miters itself against the
  earlier, the same halving plane and the rule's `Gap` as within one flange, and the corner
  relief applies there too.
- Corner closure: a feature on two legs meeting at a corner, `Butt` (the default), `Overlap` or
  `Underlap`, with the rule's `Gap`; it carries the legs' sides on to close the corner, as
  SOLIDWORKS's Closed Corner. `Butt` is both legs carried on to the plane halving the corner,
  the gap between (a miter, as the automatic one), not square ends leaving the outside corner
  open.
- A corner relief is drawn in the flat and folded back through the fold maps: the blank is what
  is cut, so its DXF carries the shape asked for.

**Ruled 2026-11-02, M2's cutout:**
- A cutout cuts normal to the sheet everywhere, across bends too: what the sketch's prism covers
  on the sheet's skin is laid flat through the element's fold map and cut through the sheet from
  there (a wall's straight through, a bend's along its radius), as the corner relief is; the hole
  is then what a laser cuts in the blank. Pocket stays as it is, cutting along the sketch.
- The sketch is taken onto the skin facing it: the side of the sheet the sketch looks at, along
  the cut's way (against the sketch's normal, as a pocket's; `Reversed` the other way).
- Through every wall and bend in the sketch's way by default (a U channel cut across both legs);
  `UpToFirst` only the first the sketch meets.

**Ruled 2026-11-02, M2's jog:**
- By default the far end stays where it was (`FixProjectedLength`): the jog adds the material
  its two bends and the run between them take, and the flat is longer by that. Off, the jog takes
  it from the wall as a fold does, the flat the part as it was. A jog usually steps a wall over
  another part's edge, where the far end's place is what has to fit.
- The offset is measured three ways (`OffsetMeasure`): `Step` (the default), how far the wall
  moves, a skin to the same skin; `Inside`, the gap between the facing skins, the step less the
  thickness; `Overall`, the outer skins, the step and the thickness.
- Otherwise as the fold: the angle 90 deg by default, the line's position that of the first
  bend, the side picked moves (else the smaller), and an offset too small for a straight run
  between the bends is refused, saying the least that fits.

**Ruled 2026-11-03, M2's bend between walls:**
- Its own feature, `PartDesign::SheetMetalJoin`, the tool **Join by Bend**.
- Two edges are picked, one on each wall, each where a skin meets the wall's end, as a flange's;
  they lie parallel. Either skin's edge at an end picks that end.
- Nothing else is set: the bend is the rule's radius, its inner skin touching both walls' inner
  skins, the inside of the turn the side the walls lie toward. Each wall is extended or trimmed
  to where the bend starts.
- Its width is the length the two edges share along the bend. Where a wall runs past that, the
  rest stays as it was, with the bend relief beside the bend where the wall is trimmed under it.
- Refused: a trim that cuts off something the wall carries, or takes all of the wall; parallel
  walls (two bends between them, as Inventor offers, maybe later: a jog steps a wall); walls
  already joined, as a loop does not lie flat without a rip; two edges sharing no length.

**Ruled 2026-11-03, M2's wrap cutout:**
- An option on the cutout, `Wrap`, not a tool of its own.
- The sketch lies on a wall's skin or on a plane parallel to it; the nearest such wall stays
  where it is, as in the flat, and the sketch is drawn on the flat in that frame: its lines
  carry across bends at their developed length.
- It cuts only; it marks nothing on the sheet.
- What of the sketch lies off the flat -- past its outline or in a gap within it -- cuts
  nothing, as a pocket over a plate's edge takes only what is there; a sketch is not refused
  for straying off the outline, which moves whenever an earlier feature is edited.

**Ruled 2026-11-03, M2's hinge and perforation cuts:**
- One feature, `PartDesign::SheetMetalBendCuts`, the tool **Bend Cuts**, on one or more bends
  picked, with a `Pattern`: `Slots`, one row of slots across the bend with bridges between, for
  bending by hand; `Staggered`, rows of slots across it, every other one shifted, the laser
  living hinge; `Sketch`, a sketch's closed shapes tiled along the bend. A wave is a sketch tile,
  not a pattern of its own.
- Drawn in the bend's strip in the flat and folded back, as the wrap cutout is, so the flat shows
  them as cut: the slot's length along the bend, the bridge between, an end margin left whole at
  each end, and for `Staggered` the rows and their spacing. A slot spans the whole strip unless
  a width is given, about the bend's middle line.
- The bend stays as it was: folded, its radius and K the rule's or its feature's. A large radius
  for a living hinge is set on the feature that made the bend. Corrected the same day: no
  feature but the hem has a radius of its own, so a bend's radius is the rule's until the bend
  table's per-bend override (M4); the ruling rested on an override that is not there.
- Refused: a pick that is not a bend, a pattern fitting no whole slot within the margins. A bend
  stays a bend however much of it is cut.

**Ruled 2026-11-03, a bend radius of a feature's own** (asked for with the bend cuts, before
the bend table's per-bend override of M4):
- Every feature that makes bends -- flange and contour flange, join by bend, fold and jog, the
  base wall's profile -- takes `UseRuleRadius` (on by default) and `BendRadius`, read only while
  the rule's is used. Turned off, `BendRadius` starts at the rule's. A radius of 0 is refused.
  The hem keeps its own `Radius`.

**Ruled 2026-11-03, the flat as a view, not a feature** (replaces 4.7's separate feature and
8.2's first two items):
- Every sheet metal feature lays itself flat when asked, not on each recompute: from its own
  shape and sheet model, cached until it recomputes, not saved. Shown by a toggle on the feature
  or on the sheet's solid, which shows its tip's flat.
- The flat's settings are the sheet's, on its solid, not one flat's: the root, chosen by the
  wall's name (`Wall1`) so every feature's flat finds it; the alignment, the grain and the
  tangent lines.
- `PartDesign::SheetMetalFlatPattern` is retired. What a bill of materials takes (the flat's
  size, area, bend count, ...) are read-only values of the solid, from its tip; the flat is a
  sub-object (`Solid.Flat`, `Flange001.Flat`) for a drawing and for links; DXF and SVG export
  from a feature or the solid. The flange handles stay as 8.2 has them.

**Ruled 2026-11-02, M2's contour flange:**
- Its own feature, as sec 5.1 lists it, sharing the flange's code: the gaps, the wall cut back under
  it with its relief, and the miters.
- The sketch is an open chain of lines on a plane square to an edge, its start on that edge; the
  first line's angle to the wall sets the first bend, and a bend is made at every corner after.
  The material lies on the side of the lines the wall's lies on: the wall's skin through the
  edge runs on as the sketch's lines, so the sketch is that skin all along. Drawn from the edge
  on the skin it bends toward, the leg's inner skin stands where the wall ended (the flange's
  `MaterialOutside`); from the other skin's edge, its outer skin does (`MaterialInside`).
- One edge or several, with the flange's gaps at each end and its miters where two edges meet;
  the same profile on each edge, taken from the edge the sketch starts on.
- Arcs in the sketch are refused, as the base wall's profile does; they come with the rolled wall
  (M5).

**Ruled 2026-10-30:** c (2), from a material card; d, M0 to M5 for the end-2026 release; e, nothing
for the addon's files; f, both; g, not run. a and b stand as proposed (a revised to (3)).

a. **The module's name.** It cannot be `src/Mod/SheetMetal`: the addon installs under that name
   and the loader lets a user's module of the same name (any case) replace a built-in one, so the
   native module would not load for anyone with the addon (3.5). Options:
   - (1) another directory and Python name, for example `SheetMetalCore`, with the workbench still
     called "Sheet Metal" in the menu;
   - (2) `SheetMetal`, and change the loader to keep a built-in module and say the addon is
     shadowed. This changes the loader for every module, including developers who replace a
     built-in one on purpose.
   Recommendation: (1). Commit area `SM:`.

   **Revised 2026-10-30**, after sec 8.1 put sheet metal in PartDesign: (3) no module of its own.
   The features are PartDesign's (`PartDesign::` types, `PartDesign_` commands), and the
   geometry layer is `PartDesign/App/SheetMetal/` (namespace `PartDesign::SheetMetal`). There is
   then nothing to shadow, no workbench beside PartDesign's, and nothing for PartDesign to load. The
   work proceeds on (3); moving it to a module of its own is cheap until the first feature is in a
   saved file.

b. **The model: the solid with ownership (4.1), or a sheet-first model.** Recommendation: the
   solid, with the sheet model as knowledge about it, so every PartDesign tool works on sheet
   metal with no twin.

c. **Where the rule's values come from.** (1) A Rule object with its own values only. (2) The
   Rule object, which may take thickness gauges, radius, K and tables from a material card with a
   new sheet metal model in the Material module. Recommendation: (2), with (1) working when no
   card is chosen.

d. **Scope for the end-2026 release.** Proposed: M0 to M4, then M5 and M6 if there is time. Or a
   cut: M0 to M2 (a part designed natively, with its flat and DXF), the rest after.

e. **The addon's documents.** A converter command run on demand (6), or nothing: users keep the
   addon for old files. Recommendation: the converter, in M6.

f. **Forms.** User tools only (any solid with a marked face), or user tools plus a built-in
   parametric set (dimple, louver, bead, lance). Recommendation: both; the built-in set is four
   small parametric solids.

g. **The addon's speed.** The timing script is ready (`~/.cache/lv-merge/sheetmetal/bench.py`) and
   needs the addon's code run, which needs the user's leave. It no longer decides anything, since
   the addon is limited in features. It would give M0's target a reference point. Recommendation:
   run it once if the user agrees; M0 does not wait for it.
