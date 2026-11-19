# GD&T / PMI: design (2026-11-19, proposed)

Status: APPROVED 2026-11-19 (section 11 decided); P0 in progress (section 10). Branch `pmi`. The user asked on 2026-11-19 for GD&T on
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

A module of its own, **`src/Mod/PMI`** (App + Gui, Python `PMI`), built the way the Surface
module is -- which lives outside PartDesign and still works inside a Body [B4]:

- **App** depends on Part only and owns the objects (section 4), the layout (6.1) and the check
  (8). Import (STEP) and TechDraw depend on it, not on PartDesign. Not in PartDesign, because an
  imported STEP part is a `Part::Feature`, not a Body, and must carry its PMI too; Assembly will
  want tolerances between parts later.
- **In the Body, referring to its solids** (revised 2026-11-19, the user's question). The
  annotation set is one of the Body's members, as its Elements group is
  (docs/AssemblyObjectModel.md sec 5.3), and its annotations refer to faces through the solid's
  `PartDesign::SolidBody`: `(Solid, "Face6")`. Measured, the three ways a reference could go:
  - `(Body, "Face6")` is wrong: a Body may hold several solids, and its own shape is its **tip's**
    solid. With a second solid added (and the tip in it), the same reference resolved to the
    *other* box's top face, silently -- nothing went missing, so nothing was flagged.
  - The set *inside* the Body linking the Body is a cycle ("The graph must be a DAG").
  - The set inside the Body linking a solid body is neither: the Body depends on the set through
    its group, the set's annotations on the solid body, never on the Body. The set names the Body
    by a hidden link (no dependency).
  So: a Body holding a set gives even a lone solid a solid body (`Part::BodyBase::
  requireSolidBody`, as sheet metal already gets one), annotations travel with the Body (copy,
  link into an assembly, export), each solid is a part with its own datum letters, and the tree
  can show each solid's annotations under it, as it shows its elements.
- **Editing an annotation rebuilds nothing downstream.** A member of a Body that changes makes
  the Body recompute; measured, that set the Body's shape again and rebuilt everything depending
  on the Body (a refine stood in for a drawing). `Body::execute` now keeps its shape when the
  tip's is the one it took last time (same TShape, transform, element map), so an annotation --
  or a VarSet, which had the same cost -- changes the Body's revision no more.
- **Beside the shape** where there is no Body: an imported `Part::Feature`, a Split piece. The
  set sits next to it, `Part` names it, references are `(Feature, "FaceN")`.
- **Gui** owns the commands and task panels, once, and offers them in two places:
  - its own **PMI workbench**, for an imported part or an assembly (and the name upstream's
    proposal and its "Mod: PMI" label use, [A8]);
  - **inside PartDesign** (and later Part and Assembly) through a `Gui::ToolContext`, as
    Surface's tool bars are offered in PartDesign while a surface group is active
    (`Surface/Gui/AppSurfaceGui.cpp:102-111`): the PMI tool bars appear while a Body's
    annotation set is active or a PMI object is in edit, so annotating a Body never means
    leaving PartDesign.

Names follow the upstream proposal (decision 1): its `PMI_AnnotationSet` is our
`PMI::AnnotationSet`, and so on; section 12 maps every part of it.

## 4. The object model

```
PMI::AnnotationSet  (in a Body, its solids' faces by solid body; beside any other shape)
 |-- Standard, Units, GeneralTolerance (ISO 2768-m, ...), Planes, Views
 |
 |-- PMI::TolerancedFeature  "toleranced feature" (= shape_aspect)        <-- shared by everything below
 |      References (faces of the part, mapped names), Kind, derived geometry
 |
 |-- PMI::Datum 'A'   -> PMI::TolerancedFeature, targets
 |-- PMI::DatumReferenceFrame  |A|B(M)|C|  -> datum features, modifiers
 |-- PMI::Dimension    size / location / basic   -> PMI::TolerancedFeature(s)
 |-- PMI::GeometricTolerance    characteristic, zone, modifiers  -> PMI::TolerancedFeature, PMI::DatumReferenceFrame
 |-- PMI::Note         free text, flag notes
 |
 |-- presentation on each item: plane, position, leaders, style (section 6)
 |-- PMI::View         camera + member list + clipping (section 6.3)
```

All are `App::DocumentObject`s (C++, App side, Python bindings by the usual XML pattern), so a
headless FreeCADCmd reads, edits, checks and exports them; the Gui only draws and edits.

### 4.1 PMI::AnnotationSet

The container and the part's settings: `Part` (a hidden link: the Body it is in, or the shape it sits beside),
`Standard` (ASME Y14.5-2018, ASME Y14.5-2009, ISO GPS -- it changes the default principle,
envelope or independency, and which symbols are legal [A6]), `GeneralTolerance` (ISO 2768 class
or a +/- default, for the title block and for dimensions without their own), `Units`,
`DecimalPlaces`. A Body has at most one, in it; another shape at most one, beside it. It is
created with the first annotation. Its group takes only PMI objects. Datum letters are each
solid's own (sec 8): a Body's two solids are two parts.

### 4.2 PMI::TolerancedFeature -- the toleranced feature

The heart of the model, because it is what survives edits.

- `References`: `PropertyLinkSubList` to the **solid's result shape** -- its
  `PartDesign::SolidBody`, not a feature inside it and not the Body (sec 3) -- with mapped
  element names. A face a later Pad modifies is still found through the element map; pointing
  at the Pad that made it would freeze it at that step.
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

Every annotation on the same faces shares one PMI::TolerancedFeature; the tools look for an existing one
before making another (STEP's uniqueness rule, [A7]).

### 4.3 PMI::Datum and PMI::DatumReferenceFrame

- `PMI::Datum`: `Name` ('A'), `Feature` (a PMI::TolerancedFeature), `Targets` (optional: point, line,
  circle, rectangle, area, each with position and size, `Movable`), and the simulated datum
  (plane/axis/point) computed into `Shape` for display and for datum-system orientation.
- `PMI::DatumReferenceFrame`: `Compartments`, up to three, each a datum or a common datum with its
  modifiers. Stored as a string list in the frame's own notation -- `["A", "B(M)", "C-D"]` --
  parsed and validated by the object: readable in the file (the save-as-directory/VCS intent),
  editable from Python, and expressions work on it. One system per distinct frame: the tools
  reuse a matching one.

### 4.4 PMI::Dimension

- `Type`: size (linear, diameter, radius, spherical, angular, thickness), location (linear
  between two features, angular, with path), or `Basic` (a TED: exact, in a box, no tolerance).
- `Features`: one PMI::TolerancedFeature for a size, two for a location.
- `Nominal`: **measured from the geometry** at recompute -- a dimension cannot disagree with
  the model. `Driver` (optional): an expression to the parameter that makes it (a Hole's
  `Diameter`, a Pad's `Length`); when set, the check (section 8) reports when the two part.
- Tolerance: `Upper`/`Lower` (+/-, unequal allowed), or `Limits`, or `Fit` (ISO 286, "H7"); the
  ISO 14405 size modifiers (`LP`, `GG`, `CT`, ...) as a list; `DecimalPlaces`.
- TechDraw's `DrawViewDimension` already has +/-, basic and format specs [B3]; section 9 makes a
  drawing dimension a view of one of these.

### 4.5 PMI::GeometricTolerance -- the feature control frame

- `Characteristic`: the 14 (12 under Y14.5-2018; the two are refused there).
- `Feature`: the toleranced PMI::TolerancedFeature.
- `Zone`: `Shape` (none, DIA, S-DIA), `Value` (a quantity, expressions allowed),
  `Modifiers` (M, L, F, P + `ProjectedLength`, T, U + `UnequalValue`, ST, CZ, ACS, LE, NC,
  LD/MD/PD, ...), `MaxValue`, `UnitBasis` (per length / area).
- `DatumSystem`: a PMI::DatumReferenceFrame, or none (form tolerances).
- `Composite`: a link to the upper frame this one is a lower segment of; `Stacked` is the
  default (independent). OCCT does not carry composite frames today [B2] -- section 5.3.

The enumerations follow OCCT's `XCAFDimTolObjects_*` where they exist (so import/export is a
table, not a translation) and CAx-IF Tables 13-15 where OCCT has gaps [A6, B2].

### 4.6 What PartDesign features contribute

A Hole already knows its thread class and fit [B4]; a Pad its length. Two ways in, both on top
of the objects above rather than instead of them:

1. **Recognise** (later, section 10): "Tolerance from features" creates PMI::TolerancedFeatures and
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
2. Create the PMI::AnnotationSet, PMI::TolerancedFeatures (sharing per face set), datum features, datum systems,
   dimensions and tolerances from the `XCAFDimTolObjects_*` objects.
3. Presentation: OCCT gives each item a plane, a text point and a presentation shape. Keep the
   imported shape as the item's graphic, flagged `Imported`, until the user edits the item;
   then the graphic is regenerated from the semantics (section 6). Graphic-only PMI (AP203/AP214,
   or AP242 without semantics) becomes `PMI::Note`s that carry only the shape.
4. Saved views (`XCAFDoc_ViewTool`) become PMI::Views.

Things to check before trusting it: the streamed import running OCCT's GD&T pass once per batch
may duplicate labels [B1]; OCCT skips datum targets on read [B2]; the reported null tolerance
values from NX/NIST files in 7.8/7.9 [A8]. The NIST CTC/FTC/STC files are the test set, and SFA's
"PMI Representation Summary" the expected counts [A7].

### 5.2 Export

1. `ExportOCAF2` adds sub-shape labels for every face a PMI::TolerancedFeature references (it already does
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

A `PMI::Layout` in PMI/App (no Qt, no Coin) turns an item into 2D strokes and fills in its
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

### 6.3 Views, and annotating in one

`PMI::View`: a camera (incl. zoom), a member list (which annotations show), which objects show,
and optional clipping -- what SolidWorks calls a 3D view, CATIA a capture, STEP a saved view [A1,
A4, A7]. Annotations are owned by the set and referenced by views, so one datum shows in several.

The working style is upstream's [A8], and every commercial program's: the set offers the
standard views (Front, Top, Right, Isometric) and the user adds their own (Section A-A).
Double-clicking a view **activates** it -- camera set, clipping applied, objects and annotations
filtered, its plane shown -- and an annotation made while a view is active joins it and takes its
plane by default. Deactivating (or leaving the workbench) restores the camera and clipping; the
annotations stay visible, with a global toggle in the View menu. With no view active everything
shows and new annotations take the default plane of 6.2.

### 6.4 Several annotations on one feature

A size dimension, its position tolerance and a datum on one hole are one stack, not three
floating labels [A8, its 5.18]: an item may be `AttachedTo` another, drawn below or beside it
with the parent's leader, and moved with it.

### 6.5 In the 3D view

A view provider per item draws the layout through a `So3DAnnotation`-style node (screen-facing
size option, depth-tested or always on top), with pre-selection and selection highlighting both
the frame and its faces (cross-highlighting is what makes PMI checkable by eye). States colour
it: broken red, remapped amber, check failures with a mark. Visibility follows the active
PMI::View, or all. The served scene carries the layout's strokes like edges.

## 7. Surviving edits

On every recompute of the part, each PMI::TolerancedFeature resolves its references in order:

1. by mapped name -- the normal case, the element map doing its job;
2. by history (`getElementHistory`), when the name changed but the element descends from one it
   had: a split face resolves to its pieces, which the feature takes as a `Group` and marks
   `Remapped`;
3. by geometry against `SavedGeometry` (same surface type and position within tolerance), as
   TechDraw's `fixByName` does [B3], marked `Remapped`;
4. otherwise `Broken`: drawn where it was, red, listed in the check, waiting for `Reattach`.

**Nothing is resolved while the solid is rolled back** -- its tip set to an earlier feature to
edit there: its faces are not the part's then, and a repair against them would be written back
and survive the roll forward. The feature keeps what it had, and resolves again when the tip
is back at the solid's last feature.

Nothing is deleted. Creo's split [A2] applies: a broken *placement* (a leader anchor) only moves
the leader to the feature's nearest face; a broken *semantic* reference is the error.

## 8. The check

A `PMI::Check` (App, so headless and testable) runs on recompute and on demand, reporting into the
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
2. **`DrawFeatureControlFrame`** (new): a frame with a leader, linked to a PMI::GeometricTolerance via
   `Source`; its content comes from the source. Unlinked, it holds its own data, so a drawing
   without a 3D model still gets real frames instead of balloons with glyphs [B3]. Built on the
   leader + tile structure of the weld symbol [B3].
3. **`DrawDatumSymbol`** (new): the datum triangle and letter, linked to a PMI::Datum.
4. **Dimensions**: `DrawViewDimension` gains `Source` (a PMI::Dimension). Linked, its tolerance,
   basic flag and decimals come from the source, its references from the source's features
   projected into the view; editing the tolerance in the drawing writes to the source, so there
   is one value.
5. A drawing's general-tolerance note can read the PMI::AnnotationSet's `GeneralTolerance`.

## 10. Phases

Each phase ends green on the full suites and with its own tests.

- **P0 Model** (headless, this branch first): section 10.1.
- **P1 Authoring**: the layout + stroke font (6.1); 3D view providers (6.5); planes, views and
  their activation (6.2-6.3); stacking (6.4); task panels -- an FCF builder with a live preview,
  face picking in the view, the tolerancing advisor offering only the characteristics the
  picked faces admit; the PMI workbench and its ToolContext in PartDesign; the query tool
  (pick a face, list what applies to it); GUI tests.
- **P2 STEP import**: the DimTolTool pass, imported presentation, views; the NIST set;
  OCCT fixes 5.3.1-2.
- **P3 STEP export**: AP242 semantic + tessellated presentation; round-trip and SFA; OCCT fix
  5.3.5.
- **P4 TechDraw**: Import Model Annotations, the FCF and datum objects, linked dimensions,
  and a page derived from chosen PMI::Views (one TechDraw view per PMI view: its direction, its
  objects, its clipping as a section, its annotations imported) [A8, its 5.22].
- **P5 Rest**: PMI::Views to STEP and composite frames (OCCT 5.3.3-4), recognition from
  features, the coverage check, general tolerances per ISO 22081, tables, the served scene and
  browser viewer, Assembly location tolerances; later surface texture (ISO 21920), welds, edges
  (ISO 13715), JT / 3D PDF / QIF.

### 10.1 P0: the model, headless

`src/Mod/PMI/App` (library `PMI`, Python `import PMI`), `BUILD_PMI` ON by default; no Gui yet.

| Object | Base | P0 properties |
|---|---|---|
| `PMI::AnnotationSet` | `App::DocumentObjectGroup` | `Part` (hidden link), `Standard` (ISO GPS, ASME Y14.5-2018, ASME Y14.5-2009), `GeneralTolerance`, `DecimalPlaces`; takes only PMI objects |
| `PMI::TolerancedFeature` | `App::DocumentObject` | `References` (LinkSubList to the solid body), `Kind`, `Shape` (resolved faces or derived axis/plane/point), `SavedGeometry`, `IsSize`, `Condition`, `ConditionMessage` |
| `PMI::Datum` | `App::DocumentObject` | `Letter`, `Feature`, `Shape` (the simulated datum) |
| `PMI::DatumReferenceFrame` | `App::DocumentObject` | `Compartments` (string list, "A", "B(M)", "C-D") |
| `PMI::Dimension` | `App::DocumentObject` | `Type`, `Features`, `Nominal` (measured), `Upper`, `Lower`, `Fit`, `Basic`, `Modifiers`, `DecimalPlaces` |
| `PMI::GeometricTolerance` | `App::DocumentObject` | `Characteristic`, `Feature`, `ZoneShape`, `Value`, `MaterialCondition`, `Modifiers`, `ProjectedLength`, `MaxValue`, `DatumSystem`, `Composite` |
| `PMI::Note` | `App::DocumentObject` | `Text` |

Plain document objects, not `Part::Feature`s: an annotation is not geometry, and must not be
picked up by exports, bounding boxes or a Body's tip. Enumerations follow
`XCAFDimTolObjects_*` (4.5). Module functions: `PMI.check(set)` -> list of (object, severity,
message), section 8; `PMI.findFeature(set, part, subs)` -> the existing toleranced feature on
those elements, or none (4.2's sharing rule).

P0's reference resolution is section 7's step 1 (by name, which the element map and `ShadowSub` already do on
recompute), step 3 (by geometry against `SavedGeometry`) and step 4 (broken, kept); step 2 (history)
follows in P1. Measurement in P0: diameter/radius of a cylinder or sphere, the width of two
opposed parallel planes, distance plane-plane, axis-axis, plane-axis, angle plane-plane.

Tests (`PMITests`, registered in `FreeCAD.__unit_test__`): building a set in a Body; sharing;
every check rule, both ways; editing the Body under its annotations -- length change, a fillet
renaming faces, a face removed -- and the states that follow; measured nominals; save and
reopen; a second solid; a rollback; an edit not rebuilding the Body's dependents.

### 10.2 P0 progress (2026-11-19)

Built: `src/Mod/PMI/App` with every object of 10.1, `PMI.check`, `PMI.findFeature`,
`PMI.findSet`, and `PMITests` (22 tests: the model, every check rule both ways, the repair).
What building it taught:

- **Step 2 (history) is P0, not P1.** A fillet on the top face's edges leaves the top and the
  side faces with new names (`Top;:H..,F` becomes `Top;:M;FLT;...`), and the link marks both
  missing. `Part::Feature::getRelatedElements(body, oldName)` finds the face each became; a face
  a later feature swallows returns nothing, which is the Broken case.
- **The link re-finds a missing name by geometry itself** (`searchElementCache`), silently. A
  feature whose name changed to one its history does not lead to is reported Remapped "by
  geometry" -- without that, a part rebuilt with new features read as Ok.
- **Derived properties must not be outputs.** With `OptimizeRecompute` (the default) a link sees
  only the linked object's non-output revision (docs/InputProperties.md sec 2): with `Shape` an
  output, a Body edit recomputed the toleranced feature and stopped, and the dimension on it kept
  its old nominal. `Shape`, `IsSize`, `Condition` and `Nominal` are read-only, not outputs.
- **A new object with nothing touched is skipped** by the same optimisation; every annotation
  calls `enforceRecompute()` when it is made.
- **OCCT 8 builders**: `BRepBuilderAPI_MakeEdge(...).Edge()` no longer converts to a
  `TopoDS_Shape`; `.Shape()` does.
- **A property named `State` hides every object's status list** in Python (`obj.State`, where
  "Touched" and "Invalid" are read); the annotations' is `Condition`.
- **Reworked the same day into the Body** (sec 3): the second-solid jump, the rollback freeze,
  the Body keeping its shape, letters per solid -- 27 tests. Traps on the way: the Body's `Shape`
  holds a *copy* of the tip's shape, so the comparison is with the tip shape taken last; a
  located tip is moved into place by a copy each time, so the comparison is before the move;
  each `getShape()` locates afresh and locations compare by identity, so transforms compare by
  value -- and `gp_XYZ::IsEqual(other, 0.0)` is false for equal values in OCCT 8.

## 11. Decisions (made 2026-11-19)

1. **Names**: the upstream proposal's (#29772, [A8]) where the concepts match --
   `PMI::AnnotationSet`, `PMI::View`, `PMI::Datum`, `PMI::DatumReferenceFrame`,
   `PMI::Dimension`, `PMI::GeometricTolerance`, `PMI::Note` -- and ours where it has no
   equivalent (`PMI::TolerancedFeature`). The user, 2026-11-19.
2. **Default standard**: ISO GPS, as upstream starts; ASME Y14.5-2018 and -2009 are a property
   of each set away. (Recommendation, taken with the user's go-ahead.)
3. **AP242 on export**: automatic whenever the document has PMI; the export says so.
   (Recommendation.)
4. **Master**: the 3D model; TechDraw shows it, linked. Editing a linked tolerance in a drawing
   edits the 3D item, so there is one value; upstream's proposal is strictly one-way, which this
   includes. (Recommendation.)
5. **Presentation per item**: one, to start; STEP's n:m written as 1:1. (Recommendation.)

## 12. The upstream proposal, mapped

Issue #29772 and its sub-issues [A8], to this design:

| Upstream | Here |
|---|---|
| 29773 PMI workbench | 3 (module, workbench, ToolContext in PartDesign) -- P1 |
| 29774 standards preferences | 4.1 `Standard`, 8 -- P0 |
| 29775 container (annotation set) | 4.1 -- P0 |
| 29776 views, activation | 6.3 -- P1 |
| 29777 clipping planes | 6.3 (a view's clipping) -- P1 |
| 29778 datums (incl. targets) | 4.3 -- P0 (targets P2, with STEP) |
| 29779 datum reference frame | 4.3 -- P0 |
| 29780 dimensional specifications | 4.4 -- P0 |
| 29781 geometric tolerances | 4.5 -- P0 |
| 29782 tolerancing advisor | 8 (rules, P0) + the task panel's offer (P1) |
| 29783 text, notes, leaders | `PMI::Note` P0; leaders 6.2, P1 |
| 29784 tables | P5 |
| 29785 construction geometry | 4.2 derived kinds (axis, centre plane, centre point), drawn in P1 |
| 29786 surface texture | P5 |
| 29787 bind to geometry | 4.2 |
| 29788 repair broken references | 7 -- states P0, Reattach P1 |
| 29789 recompute behaviour | 4.4 (nominal measured, tolerances kept), 7 |
| 29790 edit annotations | P1 task panels |
| 29791 stacking | 6.4 -- P1 |
| 29792 view membership | 6.3 -- P1 |
| 29793 validate | 8 -- P0 |
| 29794 query a feature's specifications | P1 query tool |
| 29795 TechDraw pages from views | 9 + P4 |
| 29796 STEP AP242 import | 5.1 -- P2 |
| 29797 STEP AP242 export | 5.2 -- P3 |
| 29798 semantic access for other workbenches | the Python API: every object is a plain document object with typed properties, P0 |

What this design adds over the proposal: the shared toleranced feature (4.2, STEP's
`shape_aspect`), the generated graphic from P1 rather than as a later goal (6.1), repair built
into recompute (7), ASME beside ISO (4.1), and the OCCT fork work it needs (5.3).
