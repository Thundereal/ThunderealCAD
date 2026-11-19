# GD&T / PMI: design (2026-11-19, proposed)

Status: PROPOSED, nothing built. Branch `GDandT`. The user asked on 2026-11-19 for GD&T on
PartDesign parts, imported from and exported to STEP, and linked into TechDraw, with a survey of
other CAD programs first. The survey is docs/GDandTSurvey.md, cited as [A n] (other programs,
the standards) and [B n] (our code). Section 11 lists the decisions that are the user's.

## 1. What it is for

A drawing's dimensions and feature control frames are the contract with the shop and the
inspector: which surfaces matter, by how much they may vary, and from what they are measured.
Model-based definition (MBD) puts that contract in the 3D model, where a CAM program, a CMM
program and the next CAD program can read it, instead of in a picture of the model.

Three things are wanted:

1. **Author** GD&T on a part in 3D: datums, size and location dimensions with tolerances, and
   geometric tolerances, attached to the part's faces and surviving its edits.
2. **Exchange** it through STEP AP242 as *semantic* PMI (machine-readable) with a matching
   graphic, and read what other programs write.
3. **Drawings**: TechDraw shows the same annotations in its views, linked to the 3D ones, not
   copied.

Not in scope here: tolerance stack-up analysis, CMM program output (QIF), surface texture
(ISO 1302) and weld symbols in 3D -- each fits the model below later (section 10).

## 2. What the survey settles

These are the points every program agrees on, or the standard fixes, so they are not open:

1. **Semantics and presentation are separate** [A2, A7, A9.2]. A tolerance is data (what,
   how much, from what); where its frame is drawn is presentation. STEP links them n:m; the
   graphic is regenerated from the data, never the other way.
2. **The toleranced feature is an object of its own** [A1, A7]: a set of faces (or a derived
   axis/centre plane/point) that every annotation on those faces shares. STEP's
   `shape_aspect` is exactly this, and SolidWorks's DimXpert feature.
3. **Datum feature and datum are different things** [A6]: the faces, and the exact geometry
   simulated from them.
4. **One datum system per distinct reference frame**, reused [A7].
5. **References break, and the break is shown** [A9.1]: kept, flagged, reattached by the user.
   Never silently deleted.
6. **Annotations sit on planes** with a reading direction; drawings take those parallel to the
   sheet [A1, A3, A9.3].
7. **The graphic is ours to make** [A8]: OCCT stores and writes presentation shapes but draws
   none.

And what we have makes three things cheap [B1-B4]:

- OCCT already reads the GD&T of every STEP file we import, and its AP242 semantic writer is
  complete; we drop the first and never feed the second.
- The element map gives TNP-safe face references, and TechDraw already names each projected
  edge and face after its 3D source -- the 3D-to-2D link is built.
- Import Edge Labels is a working pattern for "3D annotation info -> editable 2D objects that
  keep the user's moves".

## 3. Where it lives

The objects live in **Part** (`src/Mod/Part/App/Pmi*`, Python `Part.Pmi*`), with their tools in
PartDesign's workbench. Not in PartDesign, because an imported STEP part is a `Part::Feature`,
not a Body, and must carry its PMI too; and Assembly will want location tolerances between
parts later. A Body admits them through `Part::BodyBase::allowType(type, "Annotation")`, which
also gives them an `AuxGroup` role of their own in the tree (as surfaces have) [B4].

(Decision 1, section 11: Part versus PartDesign, and whether to adopt upstream's `PMI_` names.)

## 4. The object model

```
PmiSet  (one per part: Body, Part::Feature, or Part)
 |-- Standard, Units, GeneralTolerance (ISO 2768-m, ...), Planes, Views
 |
 |-- PmiFeature  "toleranced feature" (= shape_aspect)        <-- shared by everything below
 |      References (faces of the part, mapped names), Kind, derived geometry
 |
 |-- PmiDatumFeature 'A'   -> PmiFeature, targets
 |-- PmiDatumSystem  |A|B(M)|C|  -> datum features, modifiers
 |-- PmiDimension    size / location / basic   -> PmiFeature(s)
 |-- PmiTolerance    characteristic, zone, modifiers  -> PmiFeature, PmiDatumSystem
 |-- PmiNote         free text, flag notes
 |
 |-- presentation on each item: plane, position, leaders, style (section 6)
 |-- PmiView         camera + member list + clipping (section 6.3)
```

All are `App::DocumentObject`s (C++, App side, Python bindings by the usual XML pattern), so a
headless FreeCADCmd reads, edits, checks and exports them; the Gui only draws and edits.

### 4.1 PmiSet

The container and the part's settings: `Part` (link to the Body or shape object it annotates),
`Standard` (ASME Y14.5-2018, ASME Y14.5-2009, ISO GPS -- it changes the default principle,
envelope or independency, and which symbols are legal [A6]), `GeneralTolerance` (ISO 2768 class
or a +/- default, for the title block and for dimensions without their own), `Units`,
`DecimalPlaces`. A Body has at most one; it is created with the first annotation.

### 4.2 PmiFeature -- the toleranced feature

The heart of the model, because it is what survives edits.

- `References`: `PropertyLinkSubList` to the **part's result shape** (the Body, not a feature
  inside it) with mapped element names. Pointing at the Body means a face that a later Pad
  modifies is still found through the element map; pointing at the Pad that made it would
  freeze it at that step.
- `Kind`: `Integral` (the faces themselves), `Axis`, `CentrePlane`, `CentrePoint`, `Apex`,
  `Group` (several faces as one: "2X", a pattern), `AllAround`, `Between` (from X to Y). The
  derived kinds compute their geometry at recompute from the faces (a cylinder's axis, two
  opposed planes' median plane) and hold it in `Shape`, so a dimension can measure to an axis
  and STEP gets its `derived_shape_aspect` + constructive geometry [A7].
- `Size`: whether it is a feature of size (a hole, a boss, a slot width) -- computed from the
  faces, it decides whether M/L modifiers are legal on tolerances that reference it.
- `State`: `Ok`, `Remapped` (found, but by history or geometry, not by name -- the user should
  look), `Broken`. `SavedGeometry` keeps the last faces it resolved to, so a broken feature is
  still drawn where it was, in red, and `Reattach` is one command (section 7).

Every annotation on the same faces shares one PmiFeature; the tools look for an existing one
before making another (STEP's uniqueness rule, [A7]).

### 4.3 PmiDatumFeature and PmiDatumSystem

- `PmiDatumFeature`: `Name` ('A'), `Feature` (a PmiFeature), `Targets` (optional: point, line,
  circle, rectangle, area, each with position and size, `Movable`), and the simulated datum
  (plane/axis/point) computed into `Shape` for display and for datum-system orientation.
- `PmiDatumSystem`: `Compartments`, up to three, each a datum or a common datum with its
  modifiers. Stored as a string list in the frame's own notation -- `["A", "B(M)", "C-D"]` --
  parsed and validated by the object: readable in the file (the save-as-directory/VCS intent),
  editable from Python, and expressions work on it. One system per distinct frame: the tools
  reuse a matching one.

### 4.4 PmiDimension

- `Type`: size (linear, diameter, radius, spherical, angular, thickness), location (linear
  between two features, angular, with path), or `Basic` (a TED: exact, in a box, no tolerance).
- `Features`: one PmiFeature for a size, two for a location.
- `Nominal`: **measured from the geometry** at recompute -- a dimension cannot disagree with
  the model. `Driver` (optional): an expression to the parameter that makes it (a Hole's
  `Diameter`, a Pad's `Length`); when set, the check (section 8) reports when the two part.
- Tolerance: `Upper`/`Lower` (+/-, unequal allowed), or `Limits`, or `Fit` (ISO 286, "H7"); the
  ISO 14405 size modifiers (`LP`, `GG`, `CT`, ...) as a list; `DecimalPlaces`.
- TechDraw's `DrawViewDimension` already has +/-, basic and format specs [B3]; section 9 makes a
  drawing dimension a view of one of these.

### 4.5 PmiTolerance -- the feature control frame

- `Characteristic`: the 14 (12 under Y14.5-2018; the two are refused there).
- `Feature`: the toleranced PmiFeature.
- `Zone`: `Shape` (none, DIA, S-DIA), `Value` (a quantity, expressions allowed),
  `Modifiers` (M, L, F, P + `ProjectedLength`, T, U + `UnequalValue`, ST, CZ, ACS, LE, NC,
  LD/MD/PD, ...), `MaxValue`, `UnitBasis` (per length / area).
- `DatumSystem`: a PmiDatumSystem, or none (form tolerances).
- `Composite`: a link to the upper frame this one is a lower segment of; `Stacked` is the
  default (independent). OCCT does not carry composite frames today [B2] -- section 5.3.

The enumerations follow OCCT's `XCAFDimTolObjects_*` where they exist (so import/export is a
table, not a translation) and CAx-IF Tables 13-15 where OCCT has gaps [A6, B2].

### 4.6 What PartDesign features contribute

A Hole already knows its thread class and fit [B4]; a Pad its length. Two ways in, both on top
of the objects above rather than instead of them:

1. **Recognise** (later, section 10): "Tolerance from features" creates PmiFeatures and
   dimensions from the Body's features -- holes become size + position, patterns become groups
   -- the way DimXpert recognises model features before topology [A1].
2. **Drive**: a dimension's `Driver` binds it to the parameter, so editing the Hole's diameter
   in the task panel updates the dimension, and the check flags a dimension whose nominal and
   driver disagree.

## 5. STEP

### 5.1 Import

OCCT already fills `XCAFDoc_DimTolTool` [B1]. After `ImportOCAF2` has made the shapes, a new
pass walks it:

1. For each referenced shape label, find the sub-shape in the imported object and its element
   name -- the `findShape()` mapping `scanElementColors` uses [B1].
2. Create the PmiSet, PmiFeatures (sharing per face set), datum features, datum systems,
   dimensions and tolerances from the `XCAFDimTolObjects_*` objects.
3. Presentation: OCCT gives each item a plane, a text point and a presentation shape. Keep the
   imported shape as the item's graphic, flagged `Imported`, until the user edits the item;
   then the graphic is regenerated from the semantics (section 6). Graphic-only PMI (AP203/AP214,
   or AP242 without semantics) becomes `PmiNote`s that carry only the shape.
4. Saved views (`XCAFDoc_ViewTool`) become PmiViews.

Things to check before trusting it: the streamed import running OCCT's GD&T pass once per batch
may duplicate labels [B1]; OCCT skips datum targets on read [B2]; the reported null tolerance
values from NX/NIST files in 7.8/7.9 [A8]. The NIST CTC/FTC/STC files are the test set, and SFA's
"PMI Representation Summary" the expected counts [A7].

### 5.2 Export

1. `ExportOCAF2` adds sub-shape labels for every face a PmiFeature references (it already does
   for per-face colours [B1]).
2. Datums, dimensions and tolerances go in with `AddDatum`/`SetDatum`, `AddDimension`/
   `SetDimension`, `AddGeomTolerance`/`SetGeomTolerance`/`SetDatumToGeomTol`.
3. Presentation: our generated graphic (section 6), tessellated, on its annotation plane --
   what the CAx-IF practice and OCCT's writer both want [A7, B2].
4. **The schema is AP242 whenever the document has PMI**: OCCT writes semantic GD&T only with
   `write.step.schema` = AP242DIS, and the legacy AP214 form otherwise [B2]. The export says so
   rather than quietly writing AP214 GD&T. (Decision 3.)
5. Validated by round-trip (export, re-import, compare objects) and by SFA on the output.

### 5.3 OCCT fork work (occt, LinkVibe-801)

The survey found five gaps [B2], in the order the phases need them:

1. Datum targets skipped on read.
2. The streamed-import duplicate risk (fork code: our root-range transfer).
3. Composite frames (`geometric_tolerance_relationship`) neither read nor written, no field in
   `XCAFDimTolObjects_GeomToleranceObject`.
4. Saved views not written to STEP (`model_geometric_view` + `camera_model_d3`).
5. The presentation curve colour hard-coded white; `writeDGTsAP242` ignoring its label list.

## 6. Presentation

### 6.1 One layout, many outputs

A `PmiLayout` in Part/App (no Qt, no Coin) turns an item into 2D strokes and fills in its
annotation plane's coordinates: the frame's boxes, the characteristic glyph, DIA, the value,
modifier circles, datum letters, the datum triangle, leaders and arrowheads. Its text uses a
built-in **stroke font** (ASCII plus the GD&T symbols, ISO 3098 lettering), not a system font,
because:

- STEP needs text as strokes or triangles, and the export must work headless [A7];
- the browser viewer and the bgfx renderer get the same lines as the desktop, through the
  served scene, with nothing font-specific to port (browser/mobile portability);
- the drawing and the model then agree by construction -- the NIST mismatch class [A9.2].

Consumers: the 3D view (Coin lines, which the render cache already feeds to bgfx), the STEP
exporter (tessellated curve sets), and TechDraw, which can draw from the same layout or with its
own Qt text -- the frame's structure, not its pixels, is what must match.

### 6.2 Annotation planes and placement

Each item has `Plane` (a placement: x = reading direction, z = toward the reader [A7]),
`Position` (in the plane), and `Leaders` (each an anchor -- the element and a parameter on it --
and optional bends). A plane is one of the set's `Planes`, which are driven (by a face, a datum
plane, or one of the six standard views) or frozen [A2]. The defaults: the plane of the
toleranced face if planar, else the standard view that looks most squarely at it. The text
always reads from the front: seen from behind, it is drawn mirrored back.

### 6.3 Views

`PmiView`: a camera, a member list (which annotations show), and optional clipping -- what
SolidWorks calls a 3D view, CATIA a capture, STEP a saved view [A1, A4, A7]. Annotations are owned
by the set and referenced by views. Std_ViewFront and friends need not know about them; the view
list sits in the tree under the set.

### 6.4 In the 3D view

A view provider per item draws the layout through a `So3DAnnotation`-style node (screen-facing
size option, depth-tested or always on top), with pre-selection and selection highlighting both
the frame and its faces (cross-highlighting is what makes PMI checkable by eye). States colour
it: broken red, remapped amber, check failures with a mark. Visibility follows the active
PmiView, or all. The served scene carries the layout's strokes like edges.

## 7. Surviving edits

On every recompute of the part, each PmiFeature resolves its references in order:

1. by mapped name -- the normal case, the element map doing its job;
2. by history (`getElementHistory`), when the name changed but the element descends from one it
   had: a split face resolves to its pieces, which the feature takes as a `Group` and marks
   `Remapped`;
3. by geometry against `SavedGeometry` (same surface type and position within tolerance), as
   TechDraw's `fixByName` does [B3], marked `Remapped`;
4. otherwise `Broken`: drawn where it was, red, listed in the check, waiting for `Reattach`.

Nothing is deleted. Creo's split [A2] applies: a broken *placement* (a leader anchor) only moves
the leader to the feature's nearest face; a broken *semantic* reference is the error.

## 8. The check

A `PmiCheck` (App, so headless and testable) runs on recompute and on demand, reporting into the
task panel and the tree:

- syntax: characteristic versus zone shape (DIA on flatness), modifiers versus feature (M/L on
  a feature that is not of size), datums on form tolerances, a datum system on position, legal
  symbols for the `Standard`;
- references: datum letters used but not defined, defined twice, a datum system whose frame
  does not constrain what the characteristic needs (orientation needs a primary);
- model: a broken or remapped feature, a dimension whose nominal and `Driver` disagree, a basic
  dimension with a tolerance;
- coverage (later): faces of the part no annotation or the general tolerance covers -- the
  "fully toleranced" question DimXpert's colours answer [A1].

## 9. TechDraw

The 3D items are the master; TechDraw shows them (decision 4).

1. **Import Model Annotations** on a DrawViewPart, built the way Import Edge Labels is [B3]:
   for each PMI item whose plane is parallel to the view (or every item, if asked), find its
   faces and edges among the view's projected geometry by their source names (`getSource3d()`,
   `;HLRF:` faces), and create or refresh a 2D object linked to it. Items whose features are not
   visible in the view are left out. Re-running it updates and keeps the user's moves
   (`isMoved`, `ImportOffset`, as `DrawEdgeLabel` does).
2. **`DrawFeatureControlFrame`** (new): a frame with a leader, linked to a PmiTolerance via
   `Source`; its content comes from the source. Unlinked, it holds its own data, so a drawing
   without a 3D model still gets real frames instead of balloons with glyphs [B3]. Built on the
   leader + tile structure of the weld symbol [B3].
3. **`DrawDatumSymbol`** (new): the datum triangle and letter, linked to a PmiDatumFeature.
4. **Dimensions**: `DrawViewDimension` gains `Source` (a PmiDimension). Linked, its tolerance,
   basic flag and decimals come from the source, its references from the source's features
   projected into the view; editing the tolerance in the drawing writes to the source, so there
   is one value.
5. A drawing's general-tolerance note can read the PmiSet's `GeneralTolerance`.

## 10. Phases

Each phase ends green on the full suites and with its own tests.

- **P0 Model** (headless): PmiSet, PmiFeature (integral + axis + centre plane), datum
  feature/system, dimension, tolerance, note; persistence; reference resolution (section 7);
  the syntax check; Python API; gtests and Python tests editing a part under its annotations.
- **P1 Authoring**: layout + stroke font; 3D view providers; task panels (an FCF builder with
  a live preview, face picking in the view); planes; tools in PartDesign's toolbar; GUI tests.
- **P2 STEP import**: the DimTolTool pass, imported presentation, views; the NIST set;
  OCCT fixes 5.3.1-2.
- **P3 STEP export**: AP242 semantic + tessellated presentation; round-trip and SFA; OCCT fix
  5.3.5.
- **P4 TechDraw**: Import Model Annotations, the FCF and datum objects, linked dimensions.
- **P5 Rest**: PmiViews to STEP and composite frames (OCCT 5.3.3-4), recognition from
  features, coverage check, the served scene and browser viewer, Assembly location tolerances.

## 11. Decisions that are the user's

1. **Home and names.** Part (recommended, section 3) or PartDesign; and whether to adopt the
   upstream proposal's names and scope (#29772, `PMI_*`, ISO GPS first [A8]) so a later
   upstream exchange is easier. Recommended: our structure, their names where the concepts
   match (`PmiDatum` for their `PMI_Datum`, `PmiDatumSystem` for `PMI_DatumReferenceFrame`).
2. **Default standard**: ASME Y14.5-2018 or ISO GPS. Both are supported; this picks the default
   for new parts and the symbols the first tools show.
3. **AP242 on export**: switch the schema to AP242 automatically when the document has PMI
   (recommended; AP214 cannot carry semantics [B2]), or ask.
4. **Master**: 3D master with TechDraw as a linked view (recommended, section 9), or two-way
   with the drawing able to own tolerances the model does not have. (Unlinked 2D frames exist
   either way.)
5. **Presentation per item**: one presentation per item (recommended to start; STEP's n:m is
   written as 1:1), or several callouts of the same item on different planes from P1.
