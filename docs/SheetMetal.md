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
| **Contour flange** | An open sketch swept along one or more edges, a bend at each corner of the profile. | B1 5 (profile mode) |
| **Hem** | Flat, open, teardrop, rolled. | B1 6 |
| **Fold** | Bends a wall along a sketched line: radius, angle, which side moves, the line at the bend's start, middle or end. | B1 9 |
| **Jog** | Two opposite bends along a line, by an offset. | -- |
| **Tab** | A closed sketch on a wall's skin adds a plate on that wall. | B1 8 |
| **Extend wall** | A wall's end face pushed out by a length, or up to a face, with gaps and a clearance against walls it would meet. | B1 7 |
| **Bend between walls** | Two walls picked, joined by a bend of the rule's radius; the walls trimmed or extended to meet it. Where two sheets of a part come together. | -- (Inventor Bend, Fusion Join by Bend, Creo Merge) |
| **Rolled wall** | An open sketch of lines and arcs revolved or rolled into a cylindrical or conical sheet, its arcs developed exactly as a lofted flange's are. | -- (Inventor Contour Roll, CATIA Rolled Wall) |
| **Starter shapes** | A parametric L, U, tub, hat or box as a first feature, made of base wall and flanges. | B1 1 |
| **Lofted flange** | Between two profiles (a round to a square, a cone): developed exactly where the surface is a cone, cylinder or plane, and as conical facets where it is not, with the facet count a parameter. | -- (#603) |

### 5.2 Edges, corners and cuts

| Tool | What it does | Ref |
|---|---|---|
| **Bend relief** | Automatic on every bend that ends inside a wall: rectangle, obround or tear, from the rule; overridden per feature. | B1 5 |
| **Corner relief** | Where two or three bends meet: circle, square, tear, weld; sized absolutely or by thickness. Automatic from the rule, or a feature on picked corners. | B1 13 |
| **Corner closure** | Two flanges brought to meet at a corner: overlap, underlap or butt, with a gap. | -- |
| **Rip** | A gap cut along an edge of a closed solid, so it can unfold. | B1 15 |
| **Cutout** | A closed sketch cut normal to the sheet on every wall and bend it crosses. | B1 18 |
| **Wrap cutout** | A sketch cut as if drawn on the flat: through the flat, and folded back (refold, 4.1). | B1 17 |
| **Notch at a bend** | A small V or slot at each end of a bend line on the outline, for the press brake operator. | -- (#622) |
| **Hinge and perforation cuts** | Slots along a bend line so it can be bent by hand, or a laser living hinge: straight, tiled from a sketch, or wave; drawn in the flat. | B1 5 (perforation), B1 10 (hinge cuts) |
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
| M2 | **Edges, corners, cuts.** Hem, fold, jog, tab, contour flange, bend between walls, rolled wall; bend relief, corner relief, corner closure; hinge and perforation cuts; cutout, wrap cutout; checks (7). | per tool, as M1; a wrap cutout across two bends is the same hole in the flat as cut there directly. | L |
| M3 | **Convert and recognise.** Convert a solid or shell, rip; the geometry recogniser for imported parts. | the M1 parts exported to STEP and read back give the same flat as their features do; a hand-made shell converts and flattens to the hand value. | M |
| M4 | **Flat state, the table and drawings.** Unfold / refold of picked bends, notches, the bend table panel with overrides, cross break, multi-sheet export, the drawings of 5.6. | an override changes one bend's flat and nothing else; refold of a slot cut in the flat is the slot cut folded. | M |
| M5 | **Forms and lofted flanges.** User tools and the built-in set (gusset among them); starter shapes; tab and slot; the flat's three ways of showing a form; lofted flange developed exactly for cones and by facets otherwise. | a cone's development against the closed form; a form kept in the flat is the same solid moved. | M |
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
  - `Gap1`, `Gap2` narrow the flange at the edge's ends; the wall is cut back under it only. No
    bend relief yet (M2).
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
  - Still to come in M1: a closed sketch for the leg's shape.
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
