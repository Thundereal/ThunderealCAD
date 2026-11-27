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
    leaving PartDesign. The way in is sheet metal's (docs/SheetMetal.md sec 8.1): the Part
    Design menu's **PMI** submenu always holds Datum, Geometric tolerance, Dimension and
    Note. An annotation made in PartDesign with no set active makes its set active, so the
    first one -- which makes the Body's set -- brings the tool bar.

Names follow the upstream proposal (decision 1): its `PMI_AnnotationSet` is our
`PMI::AnnotationSet`, and so on; section 12 maps every part of it.

## 4. The object model

```
PMI::AnnotationSet  (in a Body, its solids' faces by solid body; beside any other shape)
 |-- Part, Standard, GeneralTolerance (ISO 2768-m, ...), DecimalPlaces
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

All are `App::DocumentObject`s (C++, App side; from Python their properties, and the functions
of the `PMI` module -- there is no binding class of their own), so a headless FreeCADCmd reads,
edits, checks and exports them; the Gui only draws and edits.

### 4.1 PMI::AnnotationSet

The container and the part's settings: `Part` (a hidden link: the Body it is in, or the shape it sits beside),
`Standard` (ASME Y14.5-2018, ASME Y14.5-2009, ISO GPS -- it changes the default principle,
envelope or independency, and which symbols are legal [A6]), `GeneralTolerance` (ISO 2768 class
or a +/- default, for the title block and for dimensions without their own), `DecimalPlaces`
(lengths are the document's, millimetres; a file's unit is the importer's and exporter's).
A Body has at most one, in it; another shape at most one, beside it. It is
created with the first annotation. Its group takes only PMI objects. Datum letters are each
solid's own (sec 8): a Body's two solids are two parts.

### 4.2 PMI::TolerancedFeature -- the toleranced feature

The heart of the model, because it is what survives edits.

- `References`: `PropertyLinkSubList` to the **solid's result shape** -- its
  `PartDesign::SolidBody`, not a feature inside it and not the Body (sec 3) -- with mapped
  element names: faces, or edges and vertices (a line element, a hole's rim, a corner; 10.14). A
  face a later Pad modifies is still found through the element map; pointing at the Pad that
  made it would freeze it at that step.
- `Kind`: `Integral` (the faces themselves), `Axis`, `CentrePlane`, `CentrePoint`, `Apex`,
  `Group` (several faces as one: "2X", a pattern), `AllAround`, `Between` (from X to Y). The
  derived kinds compute their geometry at recompute from the faces (a cylinder's axis, two
  opposed planes' median plane) and hold it in `Shape`, so a dimension can measure to an axis
  and STEP gets its `derived_shape_aspect` + constructive geometry [A7].
- `IsSize`: whether it is a feature of size (a hole, a boss, a slot width) -- computed from the
  faces, it decides whether M/L modifiers are legal on tolerances that reference it.
- `Condition` (with `ConditionMessage`): `Ok`, `Remapped` (found, but by history or geometry,
  not by name -- the user should look), `Broken`. `SavedGeometry` keeps the last faces it
  resolved to, so a broken feature is still drawn where it was, in red; it is reattached by
  setting `References` again (section 7; no command of its own yet).

Every annotation on the same faces shares one PMI::TolerancedFeature; the tools look for an existing one
before making another (STEP's uniqueness rule, [A7]).

### 4.3 PMI::Datum and PMI::DatumReferenceFrame

- `PMI::Datum`: `Letter` ('A'), `Feature` (a PMI::TolerancedFeature), `Targets` (optional:
  point, line,
  circle, rectangle, area, each with position and size, `Movable`), and the simulated datum
  (plane/axis/point) computed into `Shape` for display and for datum-system orientation.
- `PMI::DatumTarget` (built 2026-11-20, 10.11): each target is its own callout, not an entry of a
  list -- in STEP each is its own shape aspect with its own presentation, and a drawing gives each
  its own symbol and leader. `Number`, `TargetType` (point, line, rectangle, circle, area),
  `Target` (a placement: its centre, z out of the part, x along a line or a rectangle's length),
  `Length` (a circle's diameter), `Width`, `Movable`, `Feature` (the face it lies on; an area
  target is that face). The datum lists its targets in `Targets` and gives them their letter; a
  datum may have targets and no feature. Its `Shape` is then its targets, until the simulation of
  a datum from targets is built.
- `PMI::DatumReferenceFrame`: `Compartments`, up to three, each a datum or a common datum with its
  modifiers. Stored as a string list in the frame's own notation -- `["A", "B(M)", "C-D"]` --
  parsed and validated by the object: readable in the file (the save-as-directory/VCS intent),
  editable from Python, and expressions work on it. One system per distinct frame: the tools
  reuse a matching one.

### 4.4 PMI::Dimension

- `Type`: a size (`Diameter`, `Radius`, `SphericalDiameter`, `SphericalRadius`, `Width` -- STEP's
  thickness too --, `AngularSize`, `CurveLength`, `Depth`) or a location between two features
  (`Distance`, `Angle`). `Basic` (a bool) makes it a TED: exact, in a box, no tolerance;
  `Reference` one given for information, in brackets.
- `Features`: one PMI::TolerancedFeature for a size, two for a location.
- `Nominal`: **measured from the geometry** at recompute -- a dimension cannot disagree with
  the model. (A `Driver`, an expression to the parameter that makes it, with a check that the
  two agree, was planned and is not built.) Given as its `Limits`, the limits stay when the
  nominal is measured again: the deviations move with it.
- Tolerance: `Upper`/`Lower` (+/-, unequal allowed), or `Limits`, or `Fit` (ISO 286, "H7"); the
  ISO 14405 size modifiers (`LP`, `GG`, `CT`, ...) as a list; `DecimalPlaces`.
- TechDraw's `DrawViewDimension` already has +/-, basic and format specs [B3]; section 9 makes a
  drawing dimension a view of one of these.

### 4.5 PMI::GeometricTolerance -- the feature control frame

- `Characteristic`: the 14, concentricity and coaxiality being two names here, 15 (12 under
  Y14.5-2018; concentricity and symmetry are refused there).
- `Feature`: the toleranced PMI::TolerancedFeature.
- Zone: `ZoneShape` (none, DIA, S-DIA), `Value` (a quantity, expressions allowed),
  `Modifiers` (M, L, F, P + `ProjectedLength`, T, U + `UnequalValue`, ST, CZ, ACS, LE, NC,
  LD/MD/PD, ...), `MaxValue`, `UnitBasis` (None, Length, Square, Rectangle, Circle) with
  `UnitSize` and a rectangle's `UnitSize2`: the value holds on any part of the feature of that
  size, "0.01/25X25" (sec 5.3 item 13).
- `DatumSystem`: a PMI::DatumReferenceFrame, or none (form tolerances).
- `Composite`: a link to the upper frame this one is a lower segment of; `Stacked` is the
  default (independent). Drawn as one frame: each lower segment a row flush under the upper,
  in its plane, with no symbol or leader of its own, the upper's symbol cell going on down
  them. Read and written as AP242's `geometric_tolerance_relationship` 'composite' (sec 5.3
  item 17).

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
   imported shape as the item's graphic, flagged `UseImported`, until the user edits the item;
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
   `XCAFDimTolObjects_GeomToleranceObject`. Read and written on the fork (2026-11-23, item 17).
4. Saved views not written to STEP (`model_geometric_view` + `camera_model_d3`). Written on
   the fork (2026-11-21, sec 10.18).
5. The presentation curve colour hard-coded white; `writeDGTsAP242` ignoring its label list.
   The colour is settable on the fork (`SetGDTPresentationColor`, 2026-11-21, sec 10.17); the
   label list is not needed while an export's XCAF document holds only what it exports.

Found reading the NIST set (sec 10.9), and fixed on the fork (2026-11-20):

6. A typed `SET_REPRESENTATION_ITEM((#a,#b))` read as one null item (`RWStepAP242`): a datum
   feature on a hole in two half cylinders lost its faces.
7. A datum's faces found only through a `geometric_item_specific_usage` on its datum feature
   (`STEPCAFControl_Reader`): not through the plain `item_identified_representation_usage`, not
   on a shape aspect the datum feature relates to, not on the datum itself. Of the set's 754
   datum labels 224 had no shapes after 6, 16 after 7.

Still open: `dimensional_size_with_datum_feature` is not an entity the reader knows (STC-07's
datum features). Datum targets are read and made (item 1, sec 10.11).

Found writing (sec 10.17), and fixed on the fork (2026-11-21):

8. A graphic-only item -- a `CommonLabel` or a `DimensionPresentation` -- written without its
   annotation plane: `writeDGTsAP242` passed `HasPlane()` as "has semantics" and `false` as "has
   a plane".

Found closing P5 (sec 10.19), and fixed on the fork (2026-11-21):

9. A saved view whose `draughting_model` is also a tessellated shape representation -- the
   complex `(draughting_model representation shape_representation
   tessellated_shape_representation)`, characterized or not -- was an entity the reader did not
   know, and `ReadViews` gave up on the file at the first camera with no draughting model: six of
   the 17 AP242 files read no view (69 of 130). Both forms read as the characterized draughting
   model; a camera in none is skipped, not the file's rest.
10. Clipping planes read but never written: a view with planes is written as a
    `camera_model_d3_multi_clipping`, its planes all cutting.

Found reading FTC-06's graphics (sec 10.24), and fixed on the fork (2026-11-23):

11. `draughting_model_item_association_with_placeholder` (AP242 ed2) was an entity the reader
    did not know -- the link from a graphic to what it presents, in seven of the NIST files,
    and in FTC-06, STC-06 and STC-08 the only one (their plain associations go to geometry).
    FTC-06 read 23 graphics as notes, its dimensions bare. Read now
    (`StepAP242_DraughtingModelItemAssociationWithPlaceholder`, type 827), a semantic link
    whatever its name (STC-07's 'PPH - ...', CTC-02's ''): FTC-06 7 notes, every dimension
    with its graphic. A PMI's graphic is still the one named the link when there is one: in
    STC-06 and STC-07 the placeholder's callout holds the placeholder alone (taken first, STC-06
    read no graphic). The placeholder itself (an `annotation_placeholder_occurrence`, where a
    reader writes the PMI's text) is kept as read, not modelled.
    `test_a_graphic_by_its_placeholder`.

Found reading STC-07's tolerances (2026-11-23), and fixed on the fork (OCCT bbabd1ade7):

12. STC-07 read 10 of its 22 tolerances. A shape aspect's geometry was looked for only through
    a `geometric_item_specific_usage`: STC-07 names its surfaces by the plain
    `item_identified_representation_usage` (a set of faces), and beside it gives them a
    'semantic text' `property_definition`, which, met first, sent the aspect down the old way
    to nothing. Either usage now, first; a `draughting_model_item_association` (a usage too,
    naming the presentation) is not taken for one. Its positions' centres of symmetry are
    axes drawn in its supplemental geometry, trimmed curves in composite curves, which were
    not transferred: each curve is now an edge, bound. 19 read: the positions on the holes'
    faces, the profile of any surface on datum feature A's surfaces, datums D and E.
    `test_features_named_as_stc_07_names_them`. Left then: items 13 and 14.

Found with the same (2026-11-23), and fixed on the fork:

13. A tolerance per unit length or area -- the complex of its type, `geometric_tolerance`,
    `geometric_tolerance_with_defined_unit` and, per area,
    `geometric_tolerance_with_defined_area_unit`: CTC-03's flatness on any 0.25 x 0.25 in,
    FTC-07's and STC-07's on any circle of 1 in, FTC-10's and STC-10's straightness on any 15
    -- was an entity the reader did not know: the tolerance was not read. Read and written now
    (`StepDimTol_GeoTolAndGeoTolWthDefUnit`, type 828; its unit sizes kept as read, a plain
    `length_measure_with_unit` or NIST's complex with a `measure_representation_item`), carried
    in XCAF (`XCAFDimTolObjects_GeomToleranceUnitBasis`: None, Length, Square, Rectangle,
    Circle, and the sizes; two child labels of `XCAFDoc_GeomTolerance`), and here as
    `UnitBasis`, `UnitSize`, `UnitSize2` (sec 4.5), drawn "0.01/25X25". With modifiers or
    datums it is a complex no reader knows: written for the whole feature, the export says so.
    In passing: `StepDimTol_GeometricToleranceWithDefinedAreaUnit::HasSecondUnitSize` said the
    opposite. `test_a_tolerance_per_unit`.
14. `dimensional_size_with_datum_feature` (a datum feature that is its own size: STC-07's holes,
    datums B and C, each with its diameter) was an entity the reader did not know: the two
    datums had no faces, their diameters, Position.1 and Perpendicularity.1 (on the holes'
    centres) were not read. Read now (`StepShape_DimensionalSizeWithDatumFeature`, type 829, a
    datum feature; a case of `StepShape_DimensionalCharacteristic`): the datum on its faces,
    the size a diameter. STC-07 reads all 22 tolerances and datums A to E.
15. A graphic is a PMI's only when that PMI is read and holds it (found by dwin, SFA p9).
    Item 11 took a graphic with a placeholder association as its PMI's whether or not that PMI
    was read: CTC-03's datum feature 6, which no tolerance cites, and its flatness (item 13,
    then unread) lost their graphics. `ReadGDTs` now reads all the GD&T first, then the
    graphics: one is left to its PMI when a presentation link (named so, or with a placeholder,
    as `readAnnotation` takes one) names a PMI read that holds a graphic, else it is a graphic
    alone, by its association to its geometry. A dimension read with no value (CTC-01's four,
    given no graphic) leaves its graphic a note, as before.
16. A PMI linked only to a placeholder -- its link's callout holds the
    `annotation_placeholder_occurrence` alone, a type the toolkit does not know -- read with no
    presentation at all: STC-07 presents its 32 radii by one graphic, '32X R', the other 31 each
    by a placeholder named 'Linear Size.13' as that graphic is, its set a 'linear dimension'.
    Ours was drawn for each and written as 31 graphics of our own names and kind ('radial
    dimension'), which SFA counts as 31 radial dimensions where NIST's file has none. Read now
    as a presentation of no shape, named as the callout, its kind the name of the placeholder's
    set; the three XCAF attributes keep a presentation's name and kind without a shape. Only
    STC-07 has them: the 31 radii, three of its oriented linear dimensions, four of its
    profiles. Its placement is the PMI's plane and text point. All 32 of the radii's are at one
    place, the '32X R': a placeholder named as a graphic is of that graphic (sec 10.10, I).
    `test_a_placeholder_is_of_the_graphic_of_its_name`.
17. Composite frames, in 11 of the NIST files: the lower segment, a tolerance of its own
    related to its upper by a `geometric_tolerance_relationship` 'composite' (the upper
    relating), read as a frame of its own, its relation lost; NIST draws both segments in the
    upper's graphic and links the lower's to none, so ours was drawn and written beside it
    (STC-07's Position.5 and .8, which SFA counts as two positions more). Carried in XCAF now
    (`XCAFDoc_DimTolTool::SetCompositeUpper` / `GetCompositeUpper`, a graph node of
    `XCAFDoc::GeomTolCompositeRefGUID`), read and written. FreeCAD links the lower segment
    (`Composite`); while its upper draws the file's graphic, the lower draws nothing of its own
    and is written with no graphic, as NIST has it; an edit of either draws both from what they
    say. `test_composite_frames_as_the_file_has_them`.
18. A datum feature that is its own size (item 14: STC-07's holes, datums B and C) took the
    size's graphic ('Linear Size.1') as the datum's, the one linked to its feature; the datum
    symbol, 'Datum Feature.2', linked to the datum, was lost. Such a feature's graphic is left
    to its size now, the datum's read from the datum.
20. A PMI linked to more graphics than one kept the first and lost the rest: STC-07's radius
    is linked to four oriented dimensions and the '32X R'. A graphic whose PMI holds another is
    a graphic alone now, on that PMI's shapes (the name it holds tells them apart).
21. From the audit of 2026-11-23 (sec 10.25), in the reader and writer: a deviation is signed
    -- an upper of -0.1 was taken for unset and a range read, a lower of +0.1 came back -0.1;
    a profile all around was read and never written (an `all_around_shape_aspect` is its
    target now); the annotation plane's text point was written unscaled in a file not in
    millimetres; a datum feature that is its own size was bound to its datum and its size in
    the reader's map, the first read winning; a value's format not 'NR2 d.d' threw.
    `STEPCAFControl_Writer` is constructed on its user's stack: the two members the GD&T work
    gave it broke binaries built before (their state is behind `myGDTPresentationDM` now, the
    class as it was). `test_what_an_audit_found_lost`.
19. A value's format -- 'NR2 1.2', how many places it is written to, a
    `value_format_type_qualifier` of the measure's `qualified_representation_item` -- was read
    only on a qualified value of the plain kind, a case its complex with a measure (NIST's every
    value) never reaches, being one of the kind before it: every NIST dimension read with no
    places, written with none, a checker printing STC-06's depth .3125 where NIST's is '.31'.
    Read now from the measure's qualifiers, wherever the value is; FreeCAD's `DecimalPlaces`.

Worked around, not fixed: `GenerateTessellatedCurveSet` writes a curve that is not a line as
its B-spline's poles -- the control polygon, not the curve -- and reads an edge's curve without
its location. The exporter hands OCCT polylines and meshes instead (sec 10.17).

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

A view is active in one 3D view, not in the document: each 3D view keeps its own, so two show two
PMI views side by side, and **Show in new 3D view** (the view's context menu, `PMI_ViewNewWindow`,
or a double click with the `DoubleClickNewView` preference) opens one beside the others. A
browser's view of a served document is a view too: a callout's **PMI views** submenu makes one
active there, changing nothing of the document, so any connection may (10.16).

### 6.4 Several annotations on one feature

A size dimension, its position tolerance and a datum on one hole are one stack, not three
floating labels [A8, its 5.18]: an item may be `AttachedTo` another, drawn below or beside it
with the parent's leader, and moved with it.

### 6.5 In the 3D view

A view provider per item draws the layout through a `So3DAnnotation`-style node (screen-facing
size option, depth-tested or always on top), with pre-selection and selection highlighting both
the frame and its faces (cross-highlighting is what makes PMI checkable by eye). States colour
it: broken red, remapped amber, check failures with a mark. Visibility follows the PMI::View
active in the 3D view drawing it, or all. The served scene carries the layout's strokes like edges. A frame, a
datum's box, a dimension's value turn per frame to read upright from wherever they are seen,
as a sketch's datum label does; leaders and extension lines stay (10.15). Each fill is rimmed
by a one-pixel line (2026-11-21): a glyph's strokes, a third of a millimetre at a 3.5 mm
height, are thinner than a pixel once the view is out a little, and a fill that thin is
rasterized in dots -- the lettering was unreadable in every screenshot of a whole part, where
a STEP file's graphic, all lines, read fine. `pmi-workbench.py` checks every fill has its rim.

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
6. **A page from PMI views**: one TechDraw view per chosen PMI::View -- its camera's direction,
   the objects it shows, its clipping as a section -- with what it shows imported (10.22).

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
  its old nominal. `Shape`, `IsSize` and `Nominal` are read-only, not outputs;
  `Condition` is each recompute's (set by hand, it lasts until the next).
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

### 10.3 P1 progress: the layout (2026-11-20)

Built, headless: `PMI::Font` (osifont, the ISO 3098 font already in the tree, read with
FreeType -- no Qt -- glyphs cached per height as planar faces and placed by translation, since
OCCT 8 refuses scaling locations), `PMI::Drawing` (strokes, fills, the 14 characteristic symbols
drawn as strokes -- osifont lacks perpendicularity and parallelism -- circled modifiers, ISO
arrowheads), and `PMI::Callout`, the base of the tolerance, the datum, the dimension and the
note: `Plane`, `TextHeight`, `AutoPlace`, `ShowLeader`, the generated `Presentation` and the
CAx-IF `EquivalentText` (e.g. "position|DIA0.1(M)|A|B|C(M)" in its code points). 37 tests.

- **Default plane** (6.2): a planar face's own, a little above it; an axis looked at along
  itself, its leader to the end toward the reader (first try went to the axis's middle, inside
  the part -- seen in a rendering, not a test); otherwise the standard view most square to the
  surface at the target.
- **Stacking** (6.4, first step): callouts landing together on parallel planes take slots of
  three text heights, in the set's order, so the result does not depend on recompute order.
- A callout moved off its feature's plane draws its leader to the target's projection (CAx-IF
  sec 8.3).
- Seen, not yet done: dimension and extension lines for location dimensions (a leader for now),
  and annotations stacked onto one leader.

### 10.4 P1 progress: the GUI (2026-11-20)

Built: `PMIGui` -- a view provider drawing each callout's `Presentation` on top of the model
(`Gui::So3DAnnotation`, which the render cache takes), blue, amber when remapped, red when
broken or invalid; a toleranced feature's derived axis or centre plane drawn dashed (upstream's
construction geometry); the annotation set, whose double-click makes it active in the view and
offers the PMI tool bar in PartDesign (`Gui::ToolContext`, key "pmi"). The PMI workbench and
commands Datum, Geometric tolerance, Dimension, Note, Check: each resolves the picked faces in
C++ (`PMI.solidReference`) and writes plain Python with the names in it, so a macro replays it.
The defaults: a plane's tolerance is flatness, a cylinder's a position of its axis in a diameter
zone, anything else profile of a surface; a dimension on a hole its diameter, on two opposed
planes the width, on two features their distance or angle. Edited in the property view for now
(the FCF builder panel came next, sec 10.5). GUI test `tests/gui/pmi-workbench.py`, 14 checks.

- **The first pick on a Body without a set cannot be named** -- the solid body comes with the
  set. The command makes the set first, inside its transaction, then resolves the pick. Found as
  a hang: the refusal was a message box, which hangs Xvfb; the test now closes modals and fails.
- **A size is measured on the faces, whatever the feature derives**: a hole's feature shared
  with a position tolerance is its axis, and its diameter came out 0 (found by the GUI test).
- **A callout under the part was half hidden by it** (datum A on the bottom face, looked at
  from above): the render cache draws an `So3DAnnotation` on top only with a priority other than
  0. The view provider sets 1.

### 10.5 P1 progress: the frame builder (2026-11-20)

Built: `PMI::Advisor` (App) -- per kind of feature (plane, width, hole or axis, sphere, cone)
the characteristics that apply, the usual first; the zone shapes each allows; whether M/L may be
given; whether datums are needed, optional or forbidden. `PMI.advise(feature)` from Python.
`PMI.frame(set, ["A", "B(M)"])` finds the set's datum reference frame with these compartments,
or makes one: one per distinct frame, as STEP has it. The builder panel
(`TaskGeometricTolerance`) opens when the tolerance command runs and on a double-click: the
advised characteristics, zone, value, material condition, F/T/CZ/ST/P with the projected length,
three datum compartments with their M/L. Each change is written at once, so the frame in the
view follows, and what the check finds is shown under it; OK commits, Cancel undoes it all.

- **Every tolerance the advisor offers passes the check** (`TestAdvisor`, on a plane, a hole
  and a width): the two cannot disagree.
- **A leader to a face with a hole in it pointed at the hole**: its end was the point of the
  face nearest the box centre, on the hole's rim. On a planar face it is now the box centre if
  that is in the material, else the point of a 24 x 24 grid deepest inside the face (farthest
  from its edges). With `BRepTopAdaptor_FClass2d`, classified once per face, a recompute costs
  3.6 ms; `BRepClass_FaceClassifier` per point cost 34.
- GUI test: 20 checks -- the builder offers what a hole admits, datums A and B with MMR make
  `|A|B`, no problems found, Cancel puts the value back.

### 10.6 P1 progress: linear dimensions, stacks (2026-11-20)

Built: a width or a distance (two parallel planes, a plane and an axis, two parallel axes, a
point and anything) is drawn as a drawing draws it -- extension lines from both sides, a
dimension line with arrows (from outside, pointing in, where it is narrower than 2.5 h), the
text over its middle -- in a plane along what it measures: the standard view (top, front,
right) most square to it, read from the bottom or the right, beside the part on the view's up
side; no leader. `Callout::autoPlane()` is now virtual, so the stacking slots of sec 6.4
count each callout where it really lands. A plane moved off the measured direction falls back
to text and leader.

`AttachedTo` (sec 6.4): a callout stacked under another takes its plane, sits under its
drawing with the left edges in line, has no leader of its own, and moves with it. A link to
anything but a callout, or one making a loop, is refused at once with a warning. The commands
stack by default: `PMI.stackUnder(callout)` is the set's last callout on the same feature
(never a linear or angular dimension), written into the macro as
`_obj.AttachedTo = PMI.stackUnder(_obj)`. A hole's diameter made after its position frame
reads under it.

- Seen in the iso capture: datums A (bottom face) and B (front face) land close together near
  the front corner; the default offsets of different planes do not see each other yet.

### 10.7 P1 progress: views (2026-11-20)

Built: `PMI::View` (sec 6.3) -- a camera (`Camera`, a placement whose z points at the viewer;
`Height`, 0 to fit; `Perspective`), a `Standard` (front, top, right, isometric) that drives
the camera and turns to Custom when the camera is turned by hand, `AnnotationPlane`, and a
clipping plane (`Clip`, `ClipPlane`). Membership is on the callout: `Views`, a link list;
`members()` reads it back. A callout's first view with `AnnotationPlane` set gives its default
plane -- the view's, just in front of the feature -- and a linear dimension takes the view's
frame when the view's right runs along what it measures, else falls back to a standard one.

In the GUI the view is activated as `pmiview` (a double-click, or `setActiveObject`): the
camera goes to the view's, its clipping is applied, and only its members are drawn (a switch
over each callout's drawing). Let go, the camera and clipping come back as they were and
every callout shows. One view is active per document; activating another in the same 3D view
does not restore in between. The `PMI_View` command makes a view from the camera as it is,
in the active set (or the selection's, or the active Body's), and activates it; an annotation
made while a view is active joins it and lies in its plane.

- A callout changes plane at the recompute after its `Views` change, as for any property.
- Not done: the object filter of sec 6.3 (`Objects` -- which bodies a view shows). Closed
  since, 10.15.

### 10.8 P1 progress: what applies to a face, cross-highlighting (2026-11-20)

Built: `PMI.query(obj, subname)` (`Api::query`) -- a face, picked on the Body or named on its
solid, gives the callouts on a feature holding it ("on"), then the tolerances whose datum frame
cites a datum on it ("datum A", the letters being the solid's own); in the set's order, each
once. `Api::featuresOf` is a callout's features, a dimension's several.

Cross-highlighting (sec 6.5), one selection observer for every callout, worked out once the
selection settles: a callout selected or preselected outlines its faces (their edges, followed
within 0.2 % of their size, drawn through the model); a face selected or preselected lights the
callouts that apply to it in the selection colour. Only a part that has a set is looked at, so
hovering over other parts costs nothing. The outline lives under the callout, and FreeCAD's
whole-object selection recolours what is under a selected object: it shows light blue, not the
selection green -- distinct from the face highlight, which is kept.

`PMI_Query` opens a panel that follows the pick: the face, and what applies to it with its
text and how. A pick of no face leaves the list, so clicking a callout in it selects it --
outlining its faces -- and the list stays.

- Still open in P1: a view's object filter (sec 6.3, `Objects`; closed since, 10.15), and the
  default offsets of callouts on different planes not seeing each other (sec 10.6).

### 10.9 P2 progress: the GD&T of a STEP file, read (2026-11-20)

Built: a pass after `ImportOCAF2` has made the shapes (sec 5.1), the same whether the file is
read at once or progressively. `ImportOCAF2::addPostImport` registers a pass by name; Import
does not link PMI, so a file with GD&T and no pass registered imports PMI first. Each pass gets
the XCAF document and a way from a shape label to the object made for it; the PMI pass is
`PMI::Step::read` (`App/StepImport.cpp`).

- **Faces**: a sub-shape label's owner gives the object, the face's index in the object's shape
  (unplaced) its name. All of one annotation's faces must be on one object; when they span
  objects, faces with no surface (a file with the geometry also tessellated names each face on
  both) and faces off the solid (supplemental surfaces beside it, FTC-10) are set aside first.
- **Where the set goes**: beside the shape, in its `App::Part` when it has one -- `getSet` now
  asks for a GeoFeatureGroup before a plain group; outside the Part the features linked into it
  out of scope.
- **Datums**: OCCT makes a datum label for each time a frame cites a datum, carrying the
  feature's faces when it found them; one datum is made per letter and faces. A letter no label
  gives faces is reported once ("only frames cite it"); datum targets are counted per letter
  and not made yet.
- **Tolerances**: characteristic, zone (diameter, spherical), value, maximum, material
  condition, the modifiers OCCT reads, projected length; the frame from the datums' positions
  and modifiers, through `Api::frame`. A position, coaxiality or concentricity, or a diameter
  zone, on a cylinder or cone takes its axis.
- **Dimensions**: diameters, radii, spherical sizes, thicknesses (as widths), angular sizes,
  angles and linear distances. OCCT keeps the lower deviation as a magnitude (its writer
  negates it), so it is negated; a range becomes deviations from the value. The nominal is the
  model's: a file's value the model does not measure is reported, and the model's kept.
  "Dimension presentation" labels, and ones with no type and no value -- the file gives them
  only as a graphic -- are counted, not made.
- **Placed as the file places them**: the annotation plane with the text point as origin,
  `AutoPlace` off; the text height half the file's smallest frame (ASME: a frame is twice the
  text), so a part a metre long is not lettered at 3.5.

The model learnt three things the files use: a pattern of holes of one size ("4X") is a feature
of size; zero is a tolerance at MMC or LMC; a radius may be a fillet's, on a torus (`Primitive`
gains `Torus`). A size is now measured on the first face of its kind, so a blind hole's diameter
is its cylinder's, not its drill point's cone.

Tests: `PMITests/TestStep.py` reads NIST CTC-01 (in the tree, public): datums A, B and C (B
and C each on two half cylinders: the fork fix of sec 5.3 item 6), the six tolerances and the
eight dimensions with value as its drawing gives them, every reference on the solid, placed and
lettered as the file does. `TestNistSet`, run when `FC_PMI_NIST` names the set, reads all 17
AP242 files: each gives GD&T (but `-tg`, whose PMI is only tessellated graphics) and nothing
broken. `tests/gui/pmi-step.py`: through ImportGui, at once and progressively, the same as
headless, none invalid, drawn.

The NIST set's ledger, what is not read or is read invalid, all 17 files (2026-11-20):

| What | Count | Why |
| --- | --- | --- |
| datum targets | 63, of 8 datums | read since: 15 targets, 10.11 |
| datums only cited | 4 (B, C, G, H) | FTC-06's two, STC-07's two (sec 5.3) |
| references to edges or points | 22 | read since: 37 features on edges, none on a point, 10.14 |
| a second feature with no shape | 1 | the file gives none |
| curve lengths | 10 | no dimension type for them |
| widths on other than two planes | 11 | a depth (read since: Depth, 10.12), or across faces |
| MMR on no feature of size | 3 | slots and counterbores: read since, 10.12 |
| graphic-only dimensions | 185 | presentation, not read yet (sec 5.1 step 3); read since, 10.10 |

### 10.10 P2 progress: the file's graphics, graphic-only PMI, saved views (2026-11-20)

Built, sec 5.1 steps 3 and 4:

- **Drawn as the file draws**: each callout read keeps the file's presentation shape in
  `ImportedGraphic` (read-only, hidden, moved into the set's coordinates) and `UseImported` on;
  while it is on, `Presentation` is that shape, not the one made from the semantics. An edit
  turns it off for good: the plane, the text height, what it is attached to, or any property
  of the PMI group a user sets. A recompute, a placement by the layout, or joining a view is no
  edit. The callout's strokes follow curves now (`ShapeLines`: a curve not a line is sampled
  to 0.5% of the graphic's size), as the files' symbols are arcs and circles.
- **Graphic-only PMI** (no type and no value, or a "dimension presentation") becomes a
  `PMI::Note` carrying the graphic, labelled with the presentation's name, on its feature when
  the file names one. Notes are made after everything else is read: a graphic the same as one
  read with its semantics (same edges, same extent) is that one's, given again -- CTC-01 gives
  its three datums', an angle's and a profile's so -- and makes no note; its label shows the one
  read. The note's `Text` is empty: what it says is in its graphic.
- **Saved views** become `PMI::View`s: the file's name, `Standard` Custom, the camera looking
  along the file's direction with its up, centred on the point of the line of sight nearest the
  part, the window height as `Height`, perspective when central. `AnnotationPlane` is off: the
  callouts keep the file's planes. The view's members are the callouts its labels resolve to,
  a datum label only a frame cites through its letter.

CTC-01: 21 callouts, all drawn as the file draws them; 4 notes (two texts, two sizes given only
as graphics), 5 graphics given twice; one view, `MBD_0`, showing all 21. Its capture matches the
NIST drawing. The NIST set's 185 graphic-only dimensions: 128 notes, 45 given twice, 12 with no
graphic (counted, nothing to carry); 59 saved views over the 17 files, none invalid.

Tests: `TestModel` (a file's graphic until edited; a view membership is no edit, a value is),
`TestStep` (drawn as the file draws, edited not; the notes, none a twin of a callout; the
view), `tests/gui/pmi-step.py` (the saved view activated: its camera, its callouts drawn).

- Next in P2: datum targets (10.11); slots and counterbores as features of size; edge and
  point references.

### 10.11 P2 progress: datum targets (2026-11-20)

Built, the model of sec 4.3, made by hand and read from STEP:

- **The model**: `PMI::DatumTarget`, a callout of its own; its datum lists it in `Targets` and
  gives it its letter (a new letter touches its targets). A datum with targets needs no
  feature; its `Shape` is then the targets' (a point, a centred line of its length, a
  rectangle, a disk, an area's faces), not yet a datum simulated from them.
- **Drawn** the ASME way: a circle split across, the label (`A1`) below, the size above
  (`DIA 85` as the diameter sign, `50.8X31.75`), a plain leader to the target. On the part,
  the target itself, in its plane: a point an X, a line a phantom line, a rectangle or a circle
  a phantom outline hatched at 45 deg; an area is its faces, highlighted as any feature.
- **Placed** in the active view's plane when it has one, beside the target; else in the
  surface, beside the target and clear of its size. A target has its own point: it is never
  stacked under a callout of the same face, nor takes one under it (two targets on one face
  each keep their leader).
- **The check**: a target is a datum's (listed in its `Targets`), numbered from 1, a line has
  its length, a circle its diameter, a rectangle both sizes, an area its faces; a datum has a
  feature or targets.
- **The GUI**: `PMI_DatumTarget` puts a point target where a face is picked, z out of the part.
  Picked with a datum selected, it is that datum's next; else a new datum takes the next letter.
- **Read from STEP**: OCCT gives a label each time a frame cites a target (the set's 63 labels
  are 15 targets); one target is made per letter and number. The file's axis is no frame to
  trust -- CTC-02 gives every point (0, 0, 1), FTC-06 its lines along their normal -- so the
  part gives it: z the outward normal of the face the target lies on (within 1e-3), a line
  centred on its edge and along it. The shape the file names is often loose geometry beside
  the part -- CTC-02's points and FTC-06's edges in an empty compound, CTC-02's K1 a disk of
  its own 20 from the solid -- so the face is looked for on the part's solid first, then on the
  shape the file gives. A letter only targets establish gets a datum with no feature. Drawn as
  the file draws them (10.10).

The NIST set: 15 targets in three files, of the 8 datums they establish -- CTC-02's A (three
points), B (four), C (one), K (a circle, DIA 85); CTC-05's C and D (rectangles 50.8 x 31.75);
FTC-06's J and K (two lines each, 31.75 long). Each is on a face of the part (K1 on its own
disk), none invalid; the rest of the ledger of 10.9 is unchanged.

Tests: `TestModel.TestDatumTarget` (a datum from three points, the letter the datum's, an area
or a rectangle marked within its size, the rules, saved and restored), `TestNistSet`'s
`test_datum_targets` (each datum's targets by type and number, each on its face),
`tests/gui/pmi-targets.py` (picked on a face; a new datum, then the selected datum's next; each
with its own leader; drawn).

Open: a line target on an edge lies on both its faces and takes the first found (the file's z
runs along the edge, nothing to choose by). A movable target has no symbol of its own yet. A
datum from targets is their compound, not the plane, axis or point they establish.

- Next in P2: slots and counterbores as features of size (10.12); edge and point references.

### 10.12 P2 progress: slots, counterbores and patterns of widths as features of size (2026-11-20)

Built: what a feature of size is (ASME Y14.5 1.3.32), decided in `Geom` and shared by the
feature, the width dimension and the advisor:

- **A width** (`Geom::width`): planes in pairs facing opposite ways, each pair one width apart
  -- those facing one way, moved the width along their normal, are those facing the other. One
  pair, or a pattern ("2X"), slots or tabs, side by side or in a row; a **slot** adds its round
  ends, half cylinders of half the width with axes across the planes' normal.
- **Holes** (`Geom::holes`): cylinders grouped by axis, every axis with the same radii -- a
  counterbore's hole and bore -- with the planes across the axes (the bores' floors) and cones on
  them (a countersink, a drill point); one, or a pattern. The hole, the smallest cylinder,
  stands for it: its axis is the feature's, whichever face is listed first.
- A **Width** dimension measures a slot or a pattern of widths; the Dimension command makes one
  when the faces picked together are a width (a slot's walls and ends), not a distance between
  the first two. The advisor calls a slot a width and a counterbored hole axial.

Faces facing the same way are a depth, not a width (STC-06's and STC-10's "widths" on a hole's
cylinders are `thickness` sizes on the shape aspect of a diameter: a hole's depth). A **Depth**
dimension (2026-11-23) is one: a size of a hole, or a pattern of holes of one depth, shown
after the depth sign (U+21A7). Each hole is its cylinders on one axis; its depth runs from the
far end of the widest (a counterbore's bore, else the hole) to its floor -- a plane square to
the axis, no further from the bore's end than the bore is wide -- or is the widest's length
where there is none: a drill point's cone is no floor, so a hole's axis 36.65 long is 35 deep
(STC-10). The import makes a `thickness` a Depth when its faces are no width (two planes, a
slot) but are a hole; the export writes a Depth as a `thickness`, as NIST does. A depth given on
a counterbore's floor alone (STC-06's .5625 on two halves of one floor, its .5 on four) is of the
bores the floor closes: the part's cylinders square to it that share its edges and run out of it
the way it faces (`Geom::bores`; the hole below a counterbore's floor runs into the material, as
a through hole does at the part's face). Two faces of one plane are no width (that one measured
0, as a width). The NIST set: STC-06's 14.2875, 12.7 (two) and 4X 7.9375, STC-10's 2.0, 3X 6.0,
10.0 (two), 27.5 and 35.0 -- twelve, all Ok and as the file gives them; FTC-07's depths are
locations between two faces, read as distances, the depth sign only in their semantic text.
A depth with no graphic of its own is drawn in its hole's (sec 10.10, I). Tests:
`TestModel.test_a_holes_depth`, `TestNistSet.test_a_holes_depth`.

The NIST set: the three "MMR on no feature of size" are read now -- FTC-09's pattern of two
widths, STC-06's four counterbored holes, STC-09's slot. Tests: `TestModel` (a slot and two
slots' walls are of size and measure 4; two faces facing the same way are not; one and two
counterbored holes are, a counterbored hole and a plain one are not; the axis the hole's).

- Next in P2: edge and point references (10.14).

### 10.13 Found making the manual's media (2026-11-20)

The manual's PMI pages (thundereal-manual `partdesign/pmi`, a screenshot and a video script
per command, on a plate with counterbored holes and a slot) drove every command as a user
does, on a part whose features carry placements of their own. It found:

- **A datum target refused on any placed tip** (`bbf134dc4d`): a click reaches the face
  through the Body's tip feature, and `getGlobalPlacement(solid, root, sub)` -- the solid
  body not on that path -- applied the tip's own placement to the picked point, off the face.
  The path is followed to the solid's group, then the solid's placement applied. The GUI
  test's box sat at the origin; it now picks through a placed tip.
- **A target's datum lost when selected through a path** (`b35a1dc5cb`): a datum selected in
  the tree arrives as itself or as (Body, "PMI.Datum."), which a pick varies; only the first
  was taken out of the selection, and the face pick read the second.
- **A slot's tolerance a profile** (`bbf134dc4d`): the command's default looked at the first
  face only. A slot or a pattern of widths is now a position; a counterbored hole, whichever
  face comes first, a position of the hole's axis.
- **Callouts edge on from the top** (`6522ce5b96`, `bccbab4f81`): a slot's frame stood in a
  wall's plane, a hole's diameter in a plane facing out of the cylinder. Both are read
  looking down the slot or the hole now, as an axis already was (ASME: a size where it
  shows as a circle), the leader to the top.
- **Dimensions over one another** (`bccbab4f81`): a linear dimension went beside the faces it
  measures, so one between two holes' axes landed inside the part, over the part's length.
  It goes beside the whole part now, where dimensions landing together are slotted apart.

Still open from the same pictures: a planar callout seen from behind reads mirrored (a datum on
the end the standard isometric hides); FreeCAD's Bottom view turns a bottom face's callouts
upside down (it flips y). Both closed since, 10.15. In PartDesign a Body's first PMI set could
not be made: the tool bar was offered only once a set existed and was active. Closed: the Part
Design menu's PMI submenu, and an annotation made there making its set active (sec 3;
`GuiPMIWorkbench_tests_run`).

### 10.14 P2 progress: edge and point references (2026-11-20)

Built: a toleranced feature may be on edges and vertices of the solid, not only faces -- a line
element (ASME Y14.5 5.4.1: straightness of a line on a surface), a hole's rim, a corner, and
what a STEP file names as such (a dimension between two edges, a note on an edge).

- **The geometry** (`Geom::ofElement`): a straight edge is a `Line`, a circular one a `Circle`
  (centre, axis, radius), a vertex a `Point`; a face is what it was. A line or a circle a file
  wrote as a B-spline is one, within the edge's own tolerance (`GeomConvert_CurveToAnaCurve`):
  FTC-07's four distances and FTC-10's 35.0 and 27.5 to hole rims, cubic splines 1.7e-4 off
  their circles, were invalid ("a distance needs planes, axes or points") and are Ok, the
  nominal measured as the file gives it (2026-11-23). `sameSurface` compares
  them (a line collinear, a circle of the same centre, axis and radius, a point coincident),
  so an edge is found again **by name, by history or by geometry** as a face is, each among
  its own kind: a face is never taken for an edge. `SavedGeometry` keeps edges and vertices as
  it keeps faces, so a broken one is still drawn where it was.
- **What they measure**: a distance between two edges, an edge and a face, or points; a
  circular edge stands for its cylinder -- a diameter or a radius, an `Axis` or a
  `CentrePoint` feature. A diameter on a straight edge is invalid ("a circular edge").
- **What they take**: the advisor's characteristics for a straight edge (straightness, line
  profile, orientations, position), a circular edge (circularity, position, line profile,
  circular runout) and a vertex (position); a rim's position zone may be a diameter.
- **The commands** take edge and vertex picks: a straight edge's tolerance defaults to a
  straightness, a vertex's to a position, a rim's to a position of its axis. One straight edge
  picked for a Dimension is refused with why: its length is not a dimension type yet.
- **The import**: a shape aspect's faces, or else its edges, or else its vertices, become the
  feature's references; a circular edge is axial, as a cylinder is.
- **Drawn**: a straight edge's callout is read side on, from the top, else the front, else the
  right, its leader to the edge's middle -- not along the edge to its end, as an axis is. A
  circular edge's is read down its axis, as its hole's is. The builder names a zone as the
  characteristic makes it: two lines for a straightness or a line profile, two circles for a
  circularity or a circular runout, two surfaces otherwise. Both were found making the
  manual's pictures.

The NIST set: every one of the files' edge references is read, 37 features in 10 files, all Ok,
none invalid. On them, 29 dimensions (CTC-04's on 60 edges, CTC-05's, FTC-06's -- two axes of
rims --, FTC-07's, FTC-09's four axes of rims, FTC-10's), 11 graphic-only notes and one of
CTC-05's tolerances. None is on a vertex. The ledger's invalid list is unchanged. Tests:
`TestModel.TestEdges` (an edge and a vertex are features; distances between edges, a rim's
diameter, a straight edge's refused; the advisor; picks; an edge elsewhere untouched and found
again after its feature is rebuilt, by geometry; saved and reopened), `TestStep` (CTC-01's two
notes on edges), `tests/gui/pmi-workbench.py` (two picked edges' distance of 20, a rim's
diameter of 6, an edge's straightness between two lines).

- Next: P1's leftovers (10.15), then P3, STEP export.

### 10.15 P1 closed: callouts read upright from every side; a view's objects (2026-11-20)

**Read upright.** A callout lies in its plane, so from behind it read mirrored and from below
(FreeCAD's Bottom view turns the camera about x) upside down. It turns now as a sketch's
datum label does, per frame, in its own plane:

- The App marks what turns: `Drawing::beginUpright()`/`endUpright()` around a part of the
  layout, else the whole layout before its leader (a frame, a datum's box, a note) -- a
  linear dimension marks its value alone, its extension and dimension lines staying.
  `Presentation` is then what stays (leader, lines, marks on the part) as its first
  sub-shape, and one sub-shape per part that turns; `UprightAt` (transient, rebuilt with
  `Presentation` on restore) holds where each turns about, in the plane.
- The view draws each part in a frame of its own under an `SoAutoZoomTranslation` with
  `datumFlip` on and `scaleFactor` 0 (the model's size, no zoom): the local X or Y that
  points left or down on the screen is mirrored, about the part's middle. A part's leader
  starts on its box's edge, which a mirror about the middle keeps.
- `datumFlip` is now done by Coin itself (`SoAutoZoomTranslation::doAction`), as bgfx did
  it: a pick, the GL render cache that replays the node and a direct Coin render agree. A
  zero `flipNormal` means the node's own local Z, so a Body's placement turns it too.
  The decision is now made from the screen alone (`SoAutoZoomTranslation::datumFlips`, a
  copy in bgfx): X reads to the right, or upwards on end -- anywhere from 80 deg below the
  screen's x to 100 above, so the cut lies off the vertical where views and text often sit
  exactly; then Y lies to the left of X on the screen, never mirrored; edge on, Y points up.
  The old test (projected axes, settled by a backfacing test on the normal) read float noise
  seen edge on and undid the mirror (from the Rear, 6 of 9 callouts on the top face), and
  left a vertical value reading downwards from behind.
- The render cache's box for a draw under such a node (`SoFCRenderCache::drawBoundingBox`)
  is carried through the node's matrix, turned every way it may turn: a fit framed the
  frames' strokes at the origin, where their local frame is.

**A view's objects.** `PMI::View::Objects` (a hidden link list: the view lives in the set
inside the Body it would name): while the view is active only they show, in the 3D view it
is active in -- every other object at the top of the tree hidden, and within a group holding
one it lists the rest of the group; the PMI stays. Empty shows all. The hides are a layer of
the view's transient visibility (`View3DInventorViewer::setVisibilityLayer`, owner
"PMI::View", beside an edit session's, which come first): never written into the view's
map, so a save sees none of them, and taken back when the view is let go. Hidden first,
then fitted, so a view fitting everything frames what it shows. A STEP saved view's shapes
(`XCAFDoc_ViewTool::GetRefShapeLabel`) become its `Objects` when they name a solid:
STC-10's 13 views name its solid; STC-09's name only a compound of construction curves,
which would hide the part, and are left showing all.

Tests: `tests/gui/pmi-workbench.py` (a Coin callback pass reads each turning part's local X
and Y on the screen from the Front, Rear, Top, Bottom, Left, Right and the isometric: none
reads backwards; with the turn off, 8 of 9 do from the Rear and from below),
`tests/gui/pmi-views.py` (a view listing the Body hides a box beside it in its 3D view only,
its Visibility untouched, back when let go and hidden again at once when made active),
`TestModel.TestPresentation` (what turns: a frame whole, a dimension's value only, a file's
graphic nothing), `TestViewObjects` (a view naming the Body it is in: no loop, saved and
reopened), `TestNistSet` (a saved view's objects include a solid).

- Next: a PMI view per 3D view (10.16), then P3.

### 10.16 A PMI view per 3D view, in a new one, and in a browser's (2026-11-21)

Until now a PMI view was active per DOCUMENT: one at a time, its callouts filtered by a switch
node every 3D view shared, while its camera, clipping and objects reached only the 3D view it was
made active in. A second 3D view saw the callouts filtered and the objects not.

**Per view.** What is active is held per view (`ViewProviderView`'s list of `Active`, keyed by
the `Gui::ViewerContext` and expiring with its `lifetime()`): a desktop 3D view, or a served
client's `MirrorViewer`. Everything a PMI view hides -- the callouts it does not show, and what
its `Objects` leave out -- is one layer of that view's transient visibility
(`ViewerContext::setVisibilityLayer`, now on the base, the desktop's and the mirror's alike), so
nothing is filtered in a view the PMI view is not active in, and a save sees none of it. The
callout's switch is gone. A callout made, or taken into or out of a view, remakes the layers.
On the desktop the 3D view's active object (`pmiview`) still says what is active, the tree
highlighting it; which 3D view changed is read from their active objects, not from which window
has focus.

**A new 3D view.** `PMI_ViewNewWindow` (also the view's context menu, "Show in new 3D view")
opens a 3D view of the document -- placed as an additional view is, a split by default --
titled after the PMI view, with the view active in it. Preferences, Part/Part Design, PMI:
"Double-click opens a PMI view in a new 3D view" (`Mod/PMI/DoubleClickNewView`, off); on, a
double click opens one, or brings up the 3D view the PMI view is already active in.

**A browser's view.** A browser has no tree: a callout's context menu has a **PMI views**
submenu, each view it is in made active or let go in the view the menu was opened in, plus "Show
all annotations". These entries carry `ViewProviderDocumentObject::ViewerEntryProperty`, a new
kind for `SceneContextMenu` (`viewer`): it changes only the view it runs in and nothing of the
document, so a view-only connection may run it, as it may turn its own camera (the rule is what
an entry changes). The client's camera is its own, so it is ASKED: the host sends
`{"cmd":"camera","q":[x,y,z,w],"at":[x,y,z],"height":h}` (`ViewerContext::turnClientCamera`,
`MirrorViewer` through the serving source's control channel); the browser keeps the camera from
before the first ask and `{"cmd":"camera","restore":true}` takes it back
(`restoreClientCamera`). Its camera has a fixed field of view, so a height becomes the distance
that shows it; 0 fits what the client shows. A browser's view does not clip (a section is not
streamed). "In a new 3D view" is the desktop's alone and is not offered to a browser.

Tests (10.16): `pmi-views.py` (two 3D views side by side, the new one, the preference),
`serve-pmi-view.py` (the wire, on a view-only connection), and the browser leg
`serve-pmi-view-browser.py` with `scripts/pmiview-drive.js` (not registered: node, puppeteer-core
and a Chrome). In headless Chrome the page's picture changes by 67% when the view is made active
-- top down on the plate, the datum gone -- and is the same to the pixel once it is let go. A
menu asked for from a script needs its ray to start at the page's eye: the server reads a ray as
the pixel of the client's camera it passes through. Under `.conda/limited.sh` Chrome needs
`LIMITED_MEM_GB=256`: it reserves far more address space than 24 GB, and dies at launch.

- Next: P3, STEP export (10.17).

### 10.17 P3: the GD&T of a part, written (2026-11-21)

**The pass.** `Import::ExportOCAF2::addPostExport` is the export's twin of `addPostImport`: at
the end of `exportObjects()` every pass gets the XCAF document and the label each exported
object went to (`PostExport`). The PMI module's (`PMI::Step::write`, `StepExport.cpp`) writes,
for every annotation set whose part was exported, its datums, datum targets, tolerances,
dimensions and notes into the DimTolTool, on sub-shape labels of the exported shape
(`AddSubShape`, as per-face colours are), each with its plane, its text point and its graphic.
`Import::WriterStep` then writes AP242 whenever the document has GD&T (`Tools::hasGDT`), whatever
the preferred schema, says so in the report view, and puts the preference back (decision 3).

**What maps to what.** The import's tables, inverted: a characteristic to its
`XCAFDimTolObjects_GeomToleranceType`, a zone shape to its type of value, MMR/LMR to the
material requirement, `P` and `U` to the zone modifiers with their values, the rest of the
modifiers to OCCT's; a dimension's type, its deviations (the lower one a magnitude, as OCCT
keeps it), its decimal places and modifiers. A frame's compartments become datum labels of
their own, each at its position with its modifiers, bound to the tolerance, as OCCT reads them
(sec 10.9); the datum features are written first, as OCCT's writer takes the first label of a
letter for the feature. A note is a graphic named by its text: a `DimensionPresentation` on its
feature, a `CommonLabel` without one -- what the import reads back as a note. Not written, and
reported as such: a basic dimension (OCCT has no theoretically exact dimension), a fit (H7: not
yet), a modifier with no STEP equivalent (`UF`, `OZ`); a composite frame's lower segment is
written as a frame of its own. Saved views (PMI::View) are written too (sec 10.18).

**Which face is which.** The annotations refer to a solid body's faces (sec 3), and a Body's
shape, its solids' and its tip's are copies of one another: no TShape in common. An element is
found in the exported shape by its TShape, else by its element map name in the part's shape
(the names survive the copies), else by its place and size (centre and area, length, point) --
unique, or not at all. The export either keeps the part's placement in the file (the shape in
its own coordinates, a location on it) or bakes it into a copy (ExportKeepPlacement): both are
covered, the copy's elements found by their index in the part's placed shape.

**Where.** The annotations are in the set's coordinates -- the Body's own for a set in a Body,
else those the part is placed in -- and the file's in the exported shape's: the move from one
to the other is read off a face found in both. A graphic goes to OCCT as polylines and meshes:
its writer takes a curve that is not a line for its B-spline's control polygon and ignores an
edge's location (sec 5.3); a fill is a face of its triangles alone, so its edges are not written
twice. The graphics are in the callouts' blue, not OCCT's white; `Mod/PMI/ExportColor`,
Preferences, Part/Part Design, PMI, STEP export, names another.

**OCCT, fixed on the fork** (sec 5.3 items 5 and 8, and three found here):

- `SetGDTPresentationColor` on `STEPCAFControl_Writer`; white unless set.
- A graphic-only item written with its annotation plane (the two flags were swapped).
- `GetTessellation` put a null item in the set for a graphic with no curves (a note of fills
  alone) and for a face with no triangles: a model that cannot be written. Only the items made
  go in now.
- A graphic-only item on a part whose faces have no entity (a tessellated part, the NIST
  `-tg` file) wrote a draughting model item association of a null shape aspect, and the
  draughting model got no context: both "Null Reference" at write time. The item is the
  product's then, as a common label's is, and the model takes the product's context.
- `writeShapeAspect` returns nothing rather than a usage of a null item.

`WriterStep` now says what the writer's checks say when a write fails ("Null Reference"),
not "Cannot open file", which it said of every failure.

The import gained one thing: a graphic-only item with no feature and nothing read before it
goes beside the file's part (the `-tg` file's 56 notes were read as nothing).

**Measured.** A Body placed at (100, 50, 5) and turned 30 deg, with three datums, a flatness, a
position at MMC to A|B|C(M), a toleranced diameter, a distance and a note: written AP242 and
read back, the same objects and values, every plane within 4.7e-13 (the placement kept out) or
8.7e-12 (kept in), every graphic's extent within 9.2e-12. Every AP242 file of the NIST set, read,
written and read again, reads the same -- datums, tolerances with their frames, dimensions (the
nominal measured again, to 1e-3), datum targets, notes -- the `-tg` file's 56 notes included.

Tests: `PMITests/TestStep.py` -- `TestStepExport` (the values, the placements both ways, the
schema left alone without PMI), `TestStepRoundTrip` (CTC-01), and
`TestNistSet.test_every_ap242_file_round_trips` when `FC_PMI_NIST` names the set.

- Next: views written (sec 10.18); SFA on the output (sec 5.2 step 5); then P4, TechDraw.

### 10.18 P5: saved views, written (2026-11-21)

Each `PMI::View` of a set written becomes a saved view of the file, as the CAx-IF practice and
the NIST files have one: a `draughting_model` named for it, holding a `camera_model_d3`, the
presentations of the callouts it shows, and the shape it shows as a `mapped_item` of the
part's representation through the camera; a `mechanical_design_and_draughting_relationship`
ties it to the model of all the presentations. OCCT reads saved views but wrote none: the fork's
writer now does (`STEPCAFControl_Writer::writeViews`, sec 5.3 item 4), from the XCAF views our
exporter makes (`StepExport.cpp`, `view()`).

**The camera.** A `PMI::View` keeps the point looked at, the turn (z toward the viewer, y up)
and the height of what shows, in the document's coordinates. A STEP camera is an eye, a line of
sight and a window: the eye goes on the line through the point looked at, a part's diagonal
out from the nearest point of it; the view plane through the point looked at; the window square,
the view's height, or for a view fitting all the extent the part fills. The camera's x is the
window's left, as the NIST files have it (their `ref_direction` points left on the screen), so
that z ^ x is up. Its objects are the shapes it shows; a view showing all names the part.
Written after every set, so a view may show callouts of another set (FTC-09's views do).

**Fixed reading, on the fork:**

- The eye. `projection_point` is in the view reference system's coordinates (ISO 10303-46),
  (0, 0, 0) in every file; the reader took it for a model point, the origin, so every saved view
  was centred on a line through the model's origin -- CTC-01's 55 mm off what its author framed.
  The eye is the view reference system's origin plus that point now.
- The view volume's lengths -- its distances and window -- are in the file's unit, scaled to the
  document's as the points are; they were not (no difference in mm).
- A composite datum feature (a datum on two half cylinders: CTC-01's B and C) was bound to its
  datum only through its components, and a presentation's association names the composite: a
  view listing that datum's presentation found no datum for it.

The import gained one rule: a view naming every solid the file has shows all, and keeps
`Objects` empty -- which is what a view showing all is written as, so it reads back the same.

**Not written:** `AnnotationPlane`, which
STEP has no place for -- a file's views are read with it off. A callout that draws nothing (an
invalid one, with no graphic) is in no view in the file: a view lists presentations.

**Measured.** The test part's two views -- one from the top with its height, one turned in
perspective fitting all -- written and read back, the placement kept and not: turned the same to
1e-9, looking along the same line to 1e-6, the height kept, the same callouts. Every AP242 file
of the NIST set, read, written and read again: the same views, cameras, heights and the callouts
each shows (those drawing something).

Tests: `TestStepExport.test_views_written_and_read_back`; `Base.assertSameViews` in
`TestStepRoundTrip.test_ctc01` and `TestNistSet.test_every_ap242_file_round_trips`.

- Next: SFA on the output (sec 5.2 step 5) -- it needs Windows and Excel; then P4, TechDraw.

### 10.19 P5 closed: a view's clipping, written and read; every saved view read (2026-11-21)

**The clipping plane.** A view clipped (`Clip`) is written with its `ClipPlane` as the plane of
a `camera_model_d3_multi_clipping` (sec 5.3 item 10), in the file's coordinates as the camera
is, and read back the same: z, the plane's normal, toward the viewer; what lies in front cut
away. That is what a STEP clipping plane's normal says -- NIST FTC-11's section view MBD_B looks
along -x from the +x side, cut by x = 0 with its normal +x. A file's view with more than one
plane is read with the first, and the rest reported.

**Every saved view read.** Reading FTC-11 for its clipping found it read no view at all: its
views' draughting models are also tessellated shape representations, a complex entity the
reader did not know, and one camera with no model ended the reading of all (sec 5.3 item 9).
Six files of the set lost every view -- CTC-04, FTC-07, FTC-08, FTC-10, FTC-11, STC-06 --
69 of the 130. Every camera of the set is a view now; the round trip of each file keeps all.

**Measured.** The test part's turned view clipped by a tilted plane, written and read back
with the placement kept and not: the normal the same to 1e-9, the plane through the same point
to 1e-6; the file has one clipping camera and one plain one. FTC-11: MBD_B alone clipped, its
plane x = 0, its normal along the camera's z. Every AP242 file: as many views as cameras, and
the same views -- clipping included -- read, written and read again.

Tests: `TestStepExport.test_views_written_and_read_back`, `TestNistSet.test_a_clipped_view`,
`TestNistSet.test_every_ap242_file` (a view per camera), `Base.assertSameViews` (the clipping).

- Next: SFA on the output (sec 5.2 step 5), asked of a Windows machine with Excel; P4,
  TechDraw.

### 10.20 P4 progress: the model's callouts on a page, linked (2026-11-21)

Sec 9 items 1-3, as one object rather than two: **`TechDraw::DrawPMICallout`**, a balloon
whose `Source` is the PMI callout it shows -- a feature control frame, a datum, a datum
target, a note -- drawn as the 3D view lays it out. A balloon in all else (the view it is of,
where its leader ends, where it stands, dragging, the page's and the view's claims), which is
also what puts it in every place TechDraw handles balloons without a line of its own there.
Unlinked frames (a drawing with no model) are not built: a frame needs a PMI object to say
what it says.

**Drawn from the layout.** `QGIPMICallout` (TechDrawGui, linked to PMI when `BUILD_PMI`)
calls the callout's `layout()` at the balloon's lettering height into a `PMI::Drawing`, the
3D view's own, and turns its loose edges into one stroked path and its faces into one
even-odd filled path -- children of the balloon's label, so they move and pick with it. Path
items only: the vg page renderer captures a page from its items and drops anything a custom
`paint()` draws, and the SVG and PDF exports print the same items. `QGIViewBalloon` gained
one hook, `drawSymbol()`, its leader leaving the symbol's side.

**Import Model Annotations** (`TechDraw_ImportModelAnnotations`, the logic headless in
`TechDrawPMI.py`): for each callout of a set whose part is the view's source, whose plane
faces the view (1 deg) -- or all, asked -- and whose feature the view shows, a linked callout:
its leader on the middle of the feature's longest visible projected edge, found by geometry
(a projected edge's `sourceIndex` is its 3D edge in the source shape; that edge lies on the
feature's face), and standing off it as the frame stands in 3D, projected. Again, it follows
the model: a callout the user moved keeps its place, one whose feature is no longer shown or
whose 3D object is gone goes, nothing is made twice. A view not yet projected imports nothing.

Found on the way:

- A Body's solid is placed by the Body: `Part.getShape(..., transform=True)` of a solid
  inside one gives it unplaced; the importer places every shape by `getGlobalPlacement()`.
- A view's projected edges (`getEdgeByIndex`) are unscaled, y down; a balloon's numbers
  unscaled, y up.
- A balloon added to the page before it names its view gets no parent item, and stands at
  the page's corner on the Qt page (the vg page placed it right): the importer names the
  view first.
- `MDIViewPage::refreshSelectedFrames` ran its deferred redraw after the page's document had
  closed, on items whose objects were gone (SIGSEGV in `drawCaption`); it checks the document
  first now.

**Measured.** The test part from the top at scale 2: the flatness, the position and the note
come in, datum A (on the hidden bottom) does not, nor B (its plane edge-on) unless all are
asked for; the leaders' ends as far apart as in the model (the position's on the hole's rim,
3 from its axis, to 1e-6); frames as far apart as in 3D; moved, kept; the note deleted, its
callout goes. On the page: the SVG gains 15 paths, the vg render draws them, the flatness's
value changed in 3D changes the drawing.

**Placed clear on paper.** The view's scale spreads the model's gaps, not the frames, which
are drawn at the page's lettering height: placed from the 3D layout alone they stood on each
other and on the view's lines. Now each frame wants the place the 3D view gives it -- the
middle of its laid-out frame (`PMI.extent(callout, height)`, the layout's box in its plane) at
its own `TextHeight`, projected -- and takes the nearest place on paper clear, by 1 mm, of the
frames placed before it, of those the user moved (fixed, placed first) and of the view's
visible lines; failing within 200 tries, clear of the frames only. The search is best-first
from the wanted place over the positions just clear of whatever is in the way (left, right,
below, above each). Sizes on paper come from the same layout at the callout's `Fontsize` (the
TechDraw dimension font-size preference headless).

Found on the way:

- `UprightAt[0]` is the point a frame turns about to read upright, not its middle; the frame's
  middle is its layout's.
- A view's `Scale` is not the scale it is drawn at: with `ScaleType` Page it is the page's.
  `DrawView.getScale()` is now in Python and the importer uses it.
- A place found just clear of a line touches it, and a closed crossing test calls that in the
  way again: the search loops on one candidate. Places are taken 1e-6 beyond.
- **Headless TechDraw projected nothing.** The projection and face finding run in worker
  threads whose end a queued signal delivers; FreeCADCmd has no Qt application, so no event
  loop, and a view never got its edges. The test made one -- and, kept, a later CAM test took
  it for a GUI and aborted in `QFontDatabase`; destroyed, by `shutdown()` or by dropping it,
  PySide invalidated every QObject Python had made before (CAM's `Notification`: "Signal
  source has been deleted"). `DrawViewPart` now projects and finds faces in place when there
  is no `QCoreApplication`, as its coarse view always did; a script needs nothing.

Open: leaders may cross the view's lines; a page from PMI views (since, 10.22).

Tests: `PMITests.TestDrawing` (headless), `tests/gui/pmi-techdraw-callouts.py`.

### 10.21 P4 progress: dimensions linked, one value (2026-11-22)

Sec 9 item 4. Import Model Annotations now brings the model's PMI dimensions too, as
`DrawViewDimension`s on the view's own edges, measuring them:

- a **diameter** or **radius** where the view sees its hole end-on: a circle among the
  visible edges, on the feature's faces, its axis along the view direction;
- a **width** (two faces of one feature) or a **distance** (two features) where the view sees
  both faces edge-on (planar, square to the view within 1 deg): the longest straight visible
  edge on each, parallel to each other. Up the page it is a `DistanceX`, its text above the
  view; across, a `DistanceY`, to its right; askew, a `Distance`.

Anything else (an angle, a face seen at a slant) the view does not show true, and is left
out. The text's place is chosen clear on paper as a frame's is.

`DrawViewDimension` has `Source` (a PMI::Dimension, global link scope). Linked, at every
recompute it takes the source's `Basic` (as `TheoreticalExact`), `Upper`/`Lower` (as the
tolerances; equal and opposite as `EqualTolerance`), `DecimalPlaces` (the set's when -1, into
the format specs' precision) and `Fit` (after the value, with no tolerance). The other way, a
tolerance typed on the drawing writes `Upper`/`Lower` and clears the fit, `TheoreticalExact`
writes `Basic` -- not while syncing, restoring or undoing. One value, whichever side edits it.
Run again, a linked dimension keeps where the user put it and follows the edges it measures;
one whose 3D dimension is no longer shown is removed, one whose 3D dimension is gone stays a
plain dimension.

Found on the way:

- A dimension's tolerance spec defaulted to its value's, prefix and all: a diameter's tolerance
  read `(diameter)+0.1`. It never showed, because the default is taken at construction, before
  `Type` says diameter; a spec recomputed later does. `getDefaultFormatSpec(true)` drops the
  type's symbol now.
- The default spec is `%.Nw` (shortest form), not `%.Nf`: the precision is set in whatever
  conversion the spec has.
- `DrawViewDimension.cpp/.h` are CRLF; an edit that writes LF turns the diff into the whole
  file.

**Measured.** The test part from the top: the hole's 6 +0.1/0 (one place) on its circle, the
40 +-0.2 width between the left and right sides as a `DistanceX`; `getRawValue` 6 and 40. In
3D, Upper 0.05 changes the drawing; Fit H7 ends the spec with it; on the drawing, 0.08 writes
Upper and clears the fit, `TheoreticalExact` sets Basic; run again, none made twice and the
moved one stays; the width's 3D dimension removed, its drawing dimension stays, unlinked. On
the page, both drawn (vg render).

### 10.22 P4 progress: a page from PMI views (2026-11-22)

Sec 9 item 6. **TechDraw > Page > Page From PMI Views** (`TechDraw_PageFromPMIViews`, with PMI
views selected; `TechDrawPMI.pageFromViews` in a script) makes a page from the TechDraw
template preference. For each PMI view it adds a part view:
- looking as the view's camera does (`Direction` and `XDirection` from `Camera`, in the set's
  frame turned into the document's);
- at the objects the view shows (`Objects`, else the set's part);
- its label "<name> view", its caption the name;
- linked to the PMI view by a `PMIView` property (`App::PropertyLinkGlobal`).

A view **clipped** square to its camera becomes a `DrawViewSection`:
- cut at the clipping plane, the material in front of it taken away, as the 3D view clips;
- its base a part view seeing the cut edge-on -- the front, the top or the right, whichever
  is square to the cut -- which carries the section line.

A view clipped askew is drawn whole, with a warning.

The views go in a grid above the title block at one standard scale (1, 2 or 5 times a power
of ten) at which each takes no more than 65% of its cell. The rest of the cell is left for
the annotations.

Each view then imports what its PMI view shows -- the callouts and dimensions whose `Views`
name it -- whatever their plane, and nothing else. A run of Import Model Annotations on the
view later does the same. Headless this happens at once. In the GUI the projections come
back on their threads, so the command imports each view once its edges are in.

Frames and dimension texts are now placed:
- **on the sheet**, inside its border;
- **clear of the title block** (ISO 7200's, 180 wide, bottom right);
- **clear of the page's other views** and their frames.

A diameter's or radius's text stands level with its centre, to the right of it, moved only
sideways. TechDraw turns an aslant one along its line, and the box it was cleared for no
longer holds it.

Of a feature's edges equally long, the leader ends on the one nearest the frame.

Found on the way:

- **The importer drew every leader upside down off a symmetric part.**
  `DrawViewPart.getEdgeByIndex` returns its edge unscaled and **y up**. Its own comment said
  y down, and the importer flipped y. The test part's top view is symmetric about its middle,
  so nothing showed it; the front view's datum on the bottom face got its triangle on the
  top line. The flips are gone; the comment (four functions) says what is returned; the page
  test checks the datum's end is below the middle.
- **A section view was never cut headless.** Its cut runs on a worker whose end a queued
  signal delivers, as the projection did (10.20). `DrawViewSection::makeSectionCut` now cuts
  in place with no `QCoreApplication`. Detail views and complex sections still wait for an
  event loop (open).
- `DrawPage.addView` puts a view at the middle of the page: a view's place is set after.
- A document keeps labels unique, so a drawing view named as its PMI view became "Top001".
- A section's `getCutPieces()` is in the document's coordinates and its edges are the
  projection's sources, edge for edge: the importer reads a section through it.

**Measured.** The test part with the top and the front as PMI views:
- a page at scale 2, the two views apart and on the sheet;
- the top with its flatness, its position and the hole's diameter (not the note: not its
  view's);
- the front with datum A, its triangle on the bottom line.

The top clipped at half height:
- a section on a front base view, keeping the part below the cut (z 5 to 10);
- on it the position and the hole's diameter (6, measured on the cut), not the flatness of
  the top it has lost.

In the GUI (`tests/gui/pmi-techdraw-page-from-views.py`), the command's page with the four
views gets every view's annotations 0.5 s after the command, and the vg render draws them.

Open:
- the page does not follow a PMI view's camera changed later;
- leaders may cross lines;
- detail and complex sections headless.

- P4 slices done: callouts, placement, linked dimensions, pages from views.
- Next: the P4 manual page.

### 10.23 P4 progress: a page that reads (2026-11-22)

The manual's plate (80 x 50, datums A B C, three positions at MMC, a flatness, the length and
width) on a page from three PMI views showed what the test part had not: the views at 1:2 on
an empty sheet, frames on the dimension lines, a datum's leader through the part. Now:

**Layout.** The grid fills down the columns -- the view from above over the one from the
front, as third angle has them; a section over its base -- each row as tall as its tallest
view, each column as wide as its widest. What is left is shared evenly between the views and
the sheet's edges, not split inside each cell. The plate's views: 1:1 (was 1:2, every row
as tall as the tallest view on the page).

**Dimensions follow the 3D view.** A dimension whose plane faces the view takes its text
where the 3D view has it (`projectPoint`, offset to the view's centred lines by a visible
edge whose ends agree), as the frames do. One that does not face it, and a diameter (level
with its centre), stand where they did.

**Frames and dimensions keep off each other**, the frames having the user's places:
- dimensions are planned before the frames, and the frames keep clear of their extension
  lines -- out to where each is meant to stand and 12 mm past -- but not of their dimension
  lines, which move instead;
- each dimension is then moved out from what it measures until its text and lines clear
  the frames; failing that, on the other side; failing that, where it was meant to;
- a dimension's line is drawn 1.45 heights of its text under the text's middle (0.77 to
  the right for a DistanceY: QGIViewDimension puts it half the label's height and a line
  spacing off, the label taller than its lettering), measured on the page;
- planned the same on a second import, so a frame not moved stays where it was.

**Leaders.** A frame's place is now also one from which its leader crosses none of the
view's lines and no frame; failing that, none of the lines; failing that, as before. The
leader is taken as TechDraw draws it from a symbol: from the middle of the frame's side
nearer its end (QGIViewBalloon), to just short of the end. A later frame keeps off the
earlier leaders. So datum A on the plate's bottom face goes under the front view, not
above it with its leader through the part.

A leader to a **circle** ends where the line from its centre to the frame meets it --
pointed at the centre from the near side, as a hole's is -- not at the middle of the edge,
which is as often on the far side: the test part's position leader crossed its own hole.

**The sheet.** Each view's caption is in the way (TechDraw writes it centred under the
view's lines, in the label font), and so is all of each dimension of the other views.

Measured on the plate: 1:1; the 80 +-0.1 above the slot's frame, the 50 left of the top
view (both where the 3D view has them); datum A under the front view; no frame on a line,
a caption or a dimension.

Open:
- a leader from a frame that has no clear way to its end still crosses (the import shot's
  slot position, over the holes' frame): the frames are placed one at a time.
  Tried and reverted the same day: placing a crossed frame again, or the crossing one
  first; a pass for a leader through no frame but across lines; and, as the step search
  (each place just clear of what was in the way) runs out of places near a frame among
  many lines, a grid of places around it. On the plate every straight leader from above to
  the slot runs through the band the holes' frame stands in, so the only clear places are
  some 60 mm off -- the page then read worse (a frame under the top view, the section's
  inside its hatching). The passes rank clear before near; a cost weighing the distance
  moved against each crossing is what it needs, and on these two pictures it keeps the
  places as they are. Left as is.

**Datums square to their line** (later the same day). TechDraw turns a balloon's end
symbol along its leader, so a datum's triangle sat flat on the feature only when the
leader happened to be square to it. A datum's leader on a line now ends at the foot of the
perpendicular from where it leaves the frame -- the middle of the side nearer the line --
anywhere along the line but its last twentieth at either end. Placing the frame, a first
pass asks for that too: a place clear in every way whose leader would meet the line past its
end is moved along the line until it does not. On the plate's front view A, B and C now
each stand square on their line, the triangle flat. A datum on a circle keeps the radial
leader, square to it already.

A datum moved by the user keeps its frame where it was; its leader's end slides square to
the line from there, and the frame moves only as far as the line itself has moved across
itself. As the end slides with the frame, the offset between them no longer tells a moved
frame from an imported one: the import clears `ImportOffset` for it, which marks it placed
by the user for good (`DrawPMICallout::isMoved`).

### 10.25 An audit (2026-11-23)

Four reviews by reading, none anchored on the work: the model and geometry, the STEP import
and export, the fork's OCCT changes, the Gui with the tests and this document. The list, with
what was done of each, is `~/works/sw/setup-logs/pmi-audit-2026-11-23.md` on the development
box. Fixed, beside sec 5.3 items 20 and 21:

- **Model.** A dimension given as its limits keeps them when its nominal is measured again
  (a file's 10.00-10.05 on a model at 10 became 9.975-10.025). An angle the file gives obtuse
  stays so (two planes are at 60 and at 120). A floor's bores are found when asked, from the
  part: a document reopened had none, a depth on a floor went invalid at the first edit (and
  every feature built an edge map of the whole part at each recompute). A composite frame's
  segments are told when one joins or leaves; a lower segment is no callout taking a slot.
  The set's decimal places and standard reach its members. A hatch has at most 1000 lines.
- **Import and export.** A tolerance's datum system is made in its part's set (a stray set
  was, for supplemental geometry). A view refused leaves no object. Two tolerances of one
  value on one feature are two when their material condition, modifiers, unit or maximum
  differ. A modifier with no name here, and an unequally disposed zone, are said, not dropped
  in silence. A file of notes, targets or views alone is recomputed and summed up.
- **Gui and Python.** The frame builder discarded what it does not show at any edit -- U,
  SZ, a datum's 'M,BSC' -- and rounded a value to four places; it left a datum system behind
  for every letter typed. A PMI view removed by an undo, or with its set, gives the 3D view
  back its camera, clipping and callouts. Another tolerance's panel is not taken for one's
  own. One face picked is dimensioned by the size it has, or refused. A C++ exception under
  a `PMI.*` function is a Python error. `PMI.feature` refuses a kind that is none and a face
  the solid has not; datum letters past Z go AA, AB.
- **Tests.** `TestNistSet` was run by no full suite: `FC_PMI_NIST` was set nowhere
  (docs/Testing.md now, and the suite script). The round trip compares zones, material
  conditions, modifiers, units and decimal places -- which found the all-around loss.
  `pmi-targets.py` was on disk and registered nowhere. Temporary folders are removed.

Open, and why: two parts with one datum letter in one file (the writer knows a datum by its
letter, file-wide; the import maps targets by letter) -- a design change; two opposed coaxial
blind holes read as one depth; the quadratic `autoSlot`; a composite frame's rows each turn
upright about their own middle; a width between two faces facing the same way is accepted;
`Datum.Shape` is null after a reopen until a recompute; a perspective PMI view is fitted, not
kept; the tests need PartDesign. And one for a decision: supplemental geometry is read for
every file since sec 10.24 K, whatever `read.step.constructivegeom.relationship` says -- a
plain part with construction curves comes in with them beside its solid.

Closed 2026-11-26 (audit leftovers): the opposed blind holes are a hole each (a bore's runs
apart on one axis); `autoSlot` keeps where each callout lands through one recompute (400
self-placed tolerances 1.70 s -> 0.20 s); a composite frame's rows turn upright as one, about
the middle of the stack; a width on two faces facing the same way is refused as a step;
`Datum.Shape` and `DatumTarget.Shape` are made again on restore; a perspective view keeps the
height it shows at its focal distance; the Body-based tests skip without PartDesign.
`SavedGeometry` keeps a reference resolved to nothing in its place (an empty compound): the
ones after it were read onto the one before. The import guards each stage over the whole
file (a failure loses that stage, said in the report), and says when a file's datum frame is
one FreeCAD does not write (a letter not A to Z, more than three compartments, a datum
twice): kept as read, shown invalid. A datum feature symbol linked to its size's
`dimensional_size_with_datum_feature` only, not its `datum`, was feared lost (B6): probed on
STC-07 with datum B's link moved so, it is not.

Closed 2026-11-26 (gaps the audit's comparisons found, I14-I16). A datum of targets alone
(CTC-02, CTC-05, FTC-06) was written a datum feature of nothing: it has none now, as NIST's.
FTC-10's supplemental axes, hidden by their one line's style each, are read hidden and written
so; their colour is their line's. A callout's slot follows the ones before it in its set,
which it does not depend on: one of those edited or gone now touches the auto-placed ones
after it (moving the first of a stack had left the second a slot up). The layers of the
part's own geometry are kept: each item's layers as the file has them, by the layer's entity
(OCCT `XCAFDoc_LayerTool::AddItemLayer`; FreeCAD `StepLayers`, by element) -- XCAF's layers
merge by name, and STC-06's layer '1' is a hidden one and a shown one. A member of a curve set
the part shares with its supplemental geometry keeps its layers on the supplemental object
(`XCAFDoc_DimTolTool::SupplementalMember`: STC-06's 36 trimmed curves). Two OCCT writer
fixes came with it: a point of its own in a compound beside other shapes was not written at
all (CTC-05's, STC-06's 12), and a written point is now found as its vertex (STC-09 -e3's
hidden points written hidden). And the part's curve sets: the AsIs writer exploded a
compound into its edges, a curve set of each (a wire flattened into trimmed curves). A
compound of curves alone (a sketch) is now one curve set as NIST writes one: each wire a
composite curve, each edge a trimmed curve, each found by its shape -- NIST hides and
layers the members, never the set (STC-09's 13 composite curves and 16 trimmed curves,
FTC-10's 11 trimmed curves: hidden each as NIST's; CTC-04's 10 composite curves layered as
NIST's; `test_a_sketch_beside_a_solid_is_one_curve_set_and_a_point_is_kept`). Five of the
six files' layers hold NIST's items one for one; CTC-04's six curve sets that are the
part's own curves or points (XCAF makes them of them) are layered member by member
(`test_part_layers_as_the_file_has_them` compares the layers, not their counts). STC-09's
one layer of supplemental and part items is written as two, and two points it hides by its
layer we hide by their style too.

Closed 2026-11-27 (I11 (c) and (d)). **A shape content's looks are its parent's.** A content
(`Part::Feature` 'Expand shape contents', STC-09's compound) took the parent's face colours
when it was made and kept them: a change of the parent's reached none. It links its parent by
a hidden link alone (`_ShapeContentOwner`), which puts it in no in-list and makes it no shape
made from anything, so neither `App::AppearanceUpdater` nor the source looks reached it. Each
element of a content now takes the look its element has in the parent, whole (a content is a
piece of the parent, not a shape made from it), and the parent tells its contents when it is
drawn differently. A content's own look wins: an element it states a look for keeps it, and a
colour, transparency or material given to the whole content turns its `MapFaceColor` (an
edge's or vertex's: `MapLineColor`, `MapPointColor`) off, so the content keeps it -- turn the
property on again to follow the parent. Saved and reopened as they were drawn
(`ShapeContentsSaveTest.test_contents_follow_their_parents_looks`). **A part's members are
not thrown out for the annotations beside it.** A `GeoFeatureGroup` keeps the members it has
checked, and a change of Group checks the rest. The ones `addObjects()` adds it checks itself,
and it did not say so: at the next change they were all taken for new, and each tied to
anything outside the part by a link made since -- every member a PMI annotation beside the
part names, through its links -- was thrown out. Deleting FTC-10's nested container so dropped
most of its parent's members ('because of cross coordinate link to View001', or Datum004, or
DatumSystem008: whichever the walk met first; the import detached the container first to get
around it, which it no longer does). And a change taking a member away checked every member
again, for the same result: it checks those tied to what left, and to what it throws out
(`PartGroupModeTest.aMemberAddedByAddObjectsIsCheckedAlready`,
`aMemberLinkedFromOutsideStaysWhenAnotherLeaves`).

Closed 2026-11-27 (the layer forms left above). **CTC-04's curve sets are layered as NIST
layers them.** A curve set of the part's own curves or points has no label of its own, so its
layer is read onto each member (`StepLayers` by element), and a member on a layer of its own
too carries two. The OCCT writer gave the first of them the member's entity and the second
none -- `NCollection_Sequence::Append(sequence)` moves the items, it does not copy them -- so
four of CTC-04's assignments were written with no items at all (`()`, invalid STEP: Sketch.3's,
Sketch.18's, Sketch.22's, Sketch.37's). Each layer now gets its items, and a layer holding
every member of a set of two or more written holds the set instead (no NIST file layers all of
a set's members one by one). The same rule ends a duplicate: a supplemental curve set's layer
read onto its members was written again by them beside the set's own (CTC-04's four
'ThreadCylinder.1' layers, eight written). And a compound of points alone (a sketch of points)
is written one point set, as a compound of curves is, not merged with every other point of the
part into one (CTC-04's 30 and 12 and 'Point.1' were one set of 43). CTC-04 now writes NIST's
32 assignments, in NIST's order, each holding NIST's kinds of items, and
`test_part_layers_as_the_file_has_them` compares what each layer holds, not only that it is
there. And STC-09 -e4's one layer of 11 supplemental planes and 6 part curve sets, written as
twelve -- the part's sets on one, each plane on one of its own -- is one again: a supplemental
item's layer keeps the number of its assignment in the file as the part's item layers do
(`XCAFDoc_DimTolTool::SetSupplementalLayer`/`GetSupplementalLayer`, new overloads with an
ordinal; `StepLayer`'s third entry), and the writer adds an item to the assignment of that
number written already, hidden by it when that one is hidden.

**PMI's STEP exchange is the OCCT fork's; the rest of PMI is not** (the standing rule of
2026-11-27, reworded the same day). What reads and writes the GD&T of STEP files
(`StepImport.cpp`, `StepExport.cpp`) calls the fork's GD&T extensions of XCAF by their C++
names, and is the module PMIStep of its own, which PMI loads only where the fork says it has
them, by `XCAFDoc_DimTolTool_PMI` (TKXCAF, a version, 1; `Part::occtPMIVersion()`). Everything
else of PMI -- making and editing annotations, their display, the checks, TechDraw's model
annotations -- imports nothing the fork alone exports, so the workbench loads and works where
the OCCT libraries are upstream's. There, the GD&T of a STEP file is not read, and Import says
so once (`ImportOCAF2::runPostImport`); an export of a part with annotations writes its shapes
and says, set by set, that its PMI is not written; `PMI.stepExchange()` is `(False, why)`, and
TestStep's classes skip saying why (their shapes' test tests the fork's writer too). The core
modules load all the same: Import reaches what it needs of the fork through C functions looked
up at run time (`Part/App/OCCTFork.h`: `setOCCTGDTPresentationColor`, `addOCCTItemLayer`,
`getOCCTItemLayers`, `occtCitedDocumentsGUID`, `occtAttributeMeasuresGUID`). This module
exports `PMI_CalloutLayout` (`Callout.h`), a callout's frame for TechDraw to look up by name. An
exported fork function keeps its signature: a new parameter is a new overload beside the old
one (`SetPlaceholder`'s `theShared`). Checked with conda-forge's occt 8.0.1: of 74 binaries
PMIStep.pyd alone imports what only the fork exports (its 38 members); PMI, PMIGui and
TechDrawGui load; TestPMIApp runs 151, the 54 of TestStep skipping; and the GUI tests of the
workbench, the queries, the targets and TechDraw's callouts pass as they do on the fork
(`tests/gui/pmi-step.py` says it skips).

The NIST set the suites run on here is NIST's February 2026 update (SFA's
`Release/NIST-PMI-STEP-Files.zip`), as `docs/Testing.md` says; with it TestPMIApp's 151 tests
run and none skips (the 2021 download's STC-09 -e3 skipped two).

SFA pass 12 (dwin) on that: STC-06 and STC-10 equal NIST's graphics table, and two things it
found. A value's format was written with the digits of the millimetres a value is kept in, in a
file in inches ('NR2 3.2' for 12 in): the writer counts those of the value it writes. And one
profile graphic short in four files, which is no regression -- the count before held a
composite's lower segment drawn twice: 'Geometrical Tolerance.1' is a tolerance of the whole
part (its toleranced shape an aspect of no geometry in STC-08 and FTC-06, the product's shape in
FTC-07 and FTC-10), which OCCT does not read. Its graphic is kept now, a graphic alone, where it
was dropped in the first two. Reading it as a tolerance needs a tolerance of no faces in the
model and 'all over' in the reader and writer (built next, below). A composite's lower segment
is written linked to no graphic, as NIST's is; being composite says whose it is drawn in.

Four things decided on that (2026-11-23), and built:

- **A tolerance of the whole part.** `TolerancedFeature.Kind` has `WholePart`: the part, all its
  faces, none named (`PMI.feature(set, solid, [], "WholePart")`). It has no saved geometry and
  nothing to repair: it is the part as it is. OCCT reads a tolerance of no geometry -- on the
  product's shape, or on a shape aspect that names none -- on the part's own label, all over;
  the import makes it on the object with the part's solid. Written, its target is the product's
  shape itself, as NIST's FTC-07 has it. The 'AllOver' modifier is not kept beside it: the
  feature says so.
- **Read with or without its PMI.** Preferences, Import-Export, STEP, 'Import PMI (GD&T)'
  (`Mod/Import/ReadPMI`, on by default). Off, `STEPCAFControl_Reader`'s GDT mode is off: no
  annotation, and none of the supplemental geometry's curves, which are read for the PMI on
  them and only then (`DESTEP_Parameters::ReadSupplementalCurves`, set from the GDT mode; a
  plain `STEPControl_Reader` is as upstream's again). This closes the audit's B1.
- **A datum letter is one part's.** Two parts written to one file may each have a datum A. The
  writer keyed its datums by letter alone and took the second part's for the first's given
  again; it keys them by letter and part now (a frame's citation, which has no shape, is of the
  letter as first written -- and a datum established by targets alone, cited from another set
  before its targets come, is still theirs: CTC-02's K). The import keys targets and the
  datums they establish the same way. This closes the audit's A4.
- **Annotation placeholders written.** A PMI's placeholder (AP242 ed2: where a reader that
  draws PMI itself puts its text) is kept as `Callout.ImportedPlaceholder`, its kind and its
  name -- the name need not be its graphic's: STC-07's dimension drawn as 'Oriented Linear
  Dimension.1' has the placeholder 'Oriented Linear Dimension.5' -- and written again: a
  callout of the placeholder alone in an annotation plane of its own, linked to the PMI by a
  `draughting_model_item_association_with_placeholder`, its geometric set named by the kind
  (what SFA counts a file's graphics by). OCCT has no class for the placeholder: it is written
  as an entity of parameters (`StepData_UndefinedEntity::SetStepType`), which a draughting
  callout may now hold. STC-07's 22 kinds and names are NIST's. A file with placeholders is
  identified as AP242's second edition (`{ 1 0 10303 442 3 1 4 }`), where they are from: SFA
  tells off one that says it is the first (pass 13). Not written: the placeholder of the datum
  targets that are not read (FTC-06's two).
- **A note's words beside its geometry (pass 13).** A graphic linked both to the geometry it
  is on and to its words was read by the first link alone, and its words lost: every note of
  CTC-02, STC-08 and FTC-06 came in with none, the general notes among them -- and a note of
  no words on the product itself had nothing to link its placeholder by. The reader takes the
  words from any of the graphic's associations to a 'semantic text'. CTC-01's two labels read
  'A' and 'B' now.
- **What the second edition asks (pass 14).** Identified as the second edition, a file is
  checked by SFA for a `model_geometric_view` connecting each saved view's camera to its
  draughting model (recommended practice 9.4.2), a syntax error when missing: written, for a
  file of that edition, as an entity of its four parameters. A note's line break is written
  in the 8-bit directive, `\X\0A`, as NIST's are -- in UTF-16 SFA counts the text as Unicode;
  OCCT's writer did not know that directive (only a form `\XHH\` the standard has not) and
  doubled its backslashes, which is fixed in `StepData_StepWriter::CleanTextForSend`. And a
  note given as items with one of '/n' between each two (FTC-08, FTC-11) is read with those
  as its line breaks, not as words.
- **Every file's graphics as the file has them (after pass 14).** Compared locally, kind and
  name of every graphic and placeholder, the NIST files written again had lost ten graphics in
  six files; each now writes NIST's table exactly (`TestNistSet.assertSameGraphics`, in the
  every-file round trip). The causes, one each:
  - a dimension given no value (CTC-01's Linear Size.5 and .6, CTC-03's .10) holds no graphic,
    and its graphic, on a shape aspect a dimension is on, was taken for one of GD&T read and
    dropped: it is a graphic alone, read as a note, and has the dimension's placeholder;
  - an Invalid dimension drew nothing, so was written with no graphic (CTC-03's thickness over
    five faces, CTC-05's two): drawn as the file draws it, it is written again;
  - two items alike, each with a graphic of its own (FTC-09's Dimension.2 and .3, STC-09's
    curve length), were kept once as read twice: two graphics' names say they are two;
  - a datum target alone, `datum_target` with a usage of its own face and no feature for datum
    target relationship (FTC-10's four areas), was not read, and an area target was written as
    a datum alone (OCCT's writer is given the face now, and relates the target to its datum);
    a target joins its letter's datum on another part written beside it;
  - a 'circular curve' target (FTC-06's G and H, which only they establish) was of no type
    OCCT knew: `XCAFDimTolObjects_DatumTargetType_CircularCurve`, `DatumTarget.TargetType`
    `CircularCurve` -- a circle of its diameter that is a curve, drawn with no hatching;
  - one placeholder two PMI are linked to (STC-06's hole callouts, the diameter's and the
    depth's) was written as two: kept as shared on reading (`ImportedPlaceholder`'s third
    item), it is written once, linked to both. STC-07's 32 'Linear Size.13', each its own at
    one spot, stay 32.

### 10.24 P3: SFA on the output (2026-11-22)

NIST's STEP File Analyzer (SFA 5.54, Windows; run by a second session there) on the 19
files written from the NIST set and the plate, beside the 17 originals. The originals:
17/17 syntax-clean. Ours: 18/19 with errors, Expected PMI (SFA against NIST's drawing) 0%
against 13-55%, and SFA's analysis died on four. All in OCCT's STEP writer; fixed in the
fork (`LinkVibe-801`, fb5fe575f9):

- a tolerance's bounds untyped, `MEASURE_WITH_UNIT(0.5,#u)`: stripping "POSITIVE_" kept
  the underscore -- every toleranced dimension's tolerance lost to a checker (the 0%);
- a `RUNOUT_ZONE_DEFINITION` for every tolerance zone, two of its three attributes; a
  projected zone never written. Now each as its modifier is, a projected one with its
  length (`ProjectedLength`, now compared by the round trip: FTC-07's four, STC-09's four);
- `COMMON_DATUM_LIST(#a,#b)` for `((#a,#b))` -- where SFA died;
- a perpendicularity (and the seven other types that are a tolerance with datum
  reference themselves) written as a complex with its own supertype;
- graphics unnamed: an occurrence and its set now carry the callout's name.

Then, from SFA's source (found by the session there, and tried on a hand-edited file), what
it reads of a graphic: its kind from the name of the occurrence's tessellated geometric set
-- one of the names of the recommended practice 8.4, whole ("datum", "position", "diameter
dimension"; "datum.1" and "flatness tolerance" are rejected) -- never from the occurrence's
name, which must only be unique (SFA counts one per type and occurrence name); and the
geometry it presents only from a draughting model item association defined by a shape
aspect, beside the semantic link. Both written now (OCCT fbb2ba150c): the set named from
the tolerance's or dimension's type, an association per shape aspect of the faces.

`TestStepExport.test_written_as_the_schema_has_it` holds each. Not the writer's, still
open from the same report: what the import loses (CTC-04, FTC-10, STC-07 read no
projected zone; STC-07 22 tolerances read as 10; datum features, datum systems shared
by tolerances written one each), per-annotation colour, and views: FTC-07 and FTC-09 gain four empty views each, some views lose annotations.

The rerun (same session, same day): syntax errors gone in 15 of 19, A to D and the
association warning gone; Expected PMI up from 0% (CTC-01 36%, CTC-02 13%, CTC-04 22%,
FTC-11 18%). Two more of the writer's, fixed (OCCT e1372a48a4):

- a datum reference element's modifier written as the bare enumeration,
  `(.MAXIMUM_MATERIAL_REQUIREMENT.)` -- a member of a SELECT, which must be typed,
  `SIMPLE_DATUM_REFERENCE_MODIFIER(...)`, as a compartment's already was. SFA died on it
  in the four left (FTC-08, FTC-10, STC-08, STC-10), every common datum at MMC;
- angles: XCAF holds degrees, the writer gave them the context's radian -- CTC-01's 60 deg
  written as 60 rad. Now a `CONVERSION_BASED_UNIT('DEGREE')` from that radian, as NIST
  writes it; and a dimension's bounds typed, `PLANE_ANGLE_`/`LENGTH_MEASURE_WITH_UNIT`.
  OCCT's reader took the old file's 60 as degrees too, so the round trip never showed it.

Still open from the rerun, not the writer's bugs: the inch files (CTC-03, CTC-05, FTC-06
to -09, STC-06 to -09) written in mm, so SFA matches them only as "possible"; a value
range written as plus/minus; CTC-03's thickness written with a nominal of 0; FTC-11's
reference dimension lost; a NIST 'label' read and written as a 'note'; some dimensions'
graphics typed as another kind; a graphic-only file's types (FTC-08-e4: all 'note');
'Associated Geometry not found' still in CTC-02 and CTC-05.

The second rerun (OCCT e1372a48a4): all 19 syntax-clean, the four finished, the angles
right (CTC-01 36 -> 43 of 53, CTC-04 22 -> 27 of 45; FTC-10 23 of 32, STC-10 22 of 32).
Then, in that order:

- **G, the inch files.** Not only SFA's: written in inches (Preferences, STEP, Unit), the
  shapes were in inches and every GD&T length in mm against the same unit -- 0.127 mm
  written 0.127, read back 0.127 inch. The GD&T writer's factors carried a length factor
  of 1; now the shape writer's, and every length it writes divided by it, the graphics'
  coordinates too (OCCT 469a45b0d3). `test_in_the_files_length_unit` holds it: the inch
  file read back the same as the mm one, values, graphics and views. NIST's ten inch files
  are written in inches for SFA (the export script follows the source's unit).
- **H, what the drawing says.** A dimension given as its limits (CTC-01's 34.8-35.2) keeps
  them: `Limits`, read from a range, drawn and written as one. A dimension the model cannot
  measure (CTC-03's thickness, not two planes) keeps the file's nominal, not 0. A reference
  dimension (`Reference`: STEP's 'dimensional note' that is 'auxiliary') is drawn in
  parentheses, with no tolerance; FTC-11's is a curve's length, a type of its own now
  (`CurveLength`, after the others so saved values stay). `test_what_the_drawing_says`;
  the round trip compares `Limits` and `Reference`.
- **The angle details, with J (Associated Geometry).** An angle's dimension is named
  'angle', as the recommended practice names it (it was ''). A nominal measured again that
  agrees with the one it has to 1e-9 is left as it is: CTC-04's 90 stays the file's 90, not
  90.000000000292. 'Associated Geometry not found' was the files with datum targets: a
  target's graphic was associated with the target, which has no geometry; now with its
  feature's shape aspects, the faces' (OCCT 96ff61f16b), and a target's relationship to its
  datum described 'datum target', as NIST's files have it. `test_a_target_on_its_geometry`.
  Not changed: a graphic alone (CTC-05's two rectangular regions, read as notes on nothing)
  is still associated with an empty shape aspect, where NIST's CTC-05 writes no association
  and its CTC-02 and FTC-06 one to an empty aspect, as ours. SFA's word decides it.
- **I, a graphic's kind.** A file's graphic is written again as the kind the file named it --
  its set's name: NIST's 'label' was written 'note', STC-09's curve lengths 'curve dimension'
  where NIST draws them as 'linear dimension', and the graphic-only FTC-08-e1-tg all 'note'.
  OCCT keeps the name (`SetPresentationType` on the three XCAF objects, a child of the
  presentation's label), FreeCAD as `ImportedGraphicType`, written while the file's graphic is
  (`UseImported`): an edited one is drawn, and named, from its semantics.
  `test_graphics_typed_as_the_file_types_them`. The file's name for a graphic is kept as
  `ImportedGraphicName` and written again the same way, a note's included (it was named by its
  words): NIST names its graphics 'Linear Size.3', 'Text.1'. A PMI the file gives only a
  placeholder (sec 5.3, item 16) has `UseImported` on and no `ImportedGraphic`: ours is drawn
  where the placeholder is, and written named and of the kind the file named it. A PMI the
  file draws in another's graphic links that one, `ImportedWith`, and draws nothing of its own
  while both are as the file has them; it is written with no graphic, linked to the other's
  as what that presents too (`XCAFDoc_DimTolTool::SetPresentedWith`: the writer adds the
  association once all are written). Edited, it is drawn from what it says and the graphic
  stays (one of 32 radii edited does not make 32 callouts); in a composite frame, one drawing,
  the upper is drawn from what it says too; and when the one holding the graphic is edited,
  those drawn in it are. The import links: a placeholder to the graphic of its name -- a
  graphic alone that PMI are drawn in is no note, the first of them holds it (STC-07's '32X
  R'); a composite frame's lower segment to its upper (sec 5.3, item 17); the second
  of two PMI read with one graphic to the first (STC-06's hole callouts, a diameter's and its
  depth's: written twice before, as two graphics); a depth with no graphic to its hole's
  diameter or radius, a depth being lettered after its hole's size (STC-10's eight, which
  had graphics of their own written for them). STC-06 and STC-10 written again have NIST's
  graphics, each the kind and name the file has (dwin, SFA p11).
- **I2, a note's words.** Every NIST AP242 file gives a note's or label's words as a 'semantic
  text': a property of what it is on (a datum feature, a dimension's feature, the product), its
  words a representation's descriptive items, the graphic its presentation. Neither side read
  or wrote it: the notes came in empty. Read now into the note's `Text`, a line each, on the
  faces of what it is of; written for every note with words, the semantic link to the text, the
  geometry link to its faces; non-ASCII in `\X2\` (the counterbore sign, a statistical
  tolerance's). `test_notes_say_what_the_file_says`. Not yet: a semantic text on a dimension's
  feature presented by the dimension's own graphic (FTC-07's '.30 +-.02') is read as that
  graphic given twice, so kept only as the dimension; NIST's placeholders
  (`DRAUGHTING_MODEL_ITEM_ASSOCIATION_WITH_PLACEHOLDER`, CTC-02, FTC-06, STC-06, -08, -10)
  OCCT does not read.
- **K, supplemental geometry.** CTC-04's thread diameter (12), its location 25 from a face and
  its position (0.75) are on curve sets of a constructive geometry representation -- the
  threads' rims -- which OCCT did not transfer (it made planes of its axis placements only):
  no shape, no dimension. FTC-10's three threads (3.0, position 0.8) the same. OCCT now
  transfers a constructive geometry representation's geometric sets as curves, always, its
  axis placements planes as before only when asked (`STEPControl_ActorRead`). FreeCAD
  imports them beside the solid, in its App::Part; an object with no solid, there beside one
  solid, has its PMI in that solid's set (`partOf`), so a location between the two is one
  part's. Circles a plane is square to are where they lie, not an axis: the 25 is plane to
  plane. Written, an element on another object exported beside the set's part is labelled on
  that object's shape: the round trip keeps them, and the Shell's profile that CTC-04 already
  read, now in the solid's set. `test_on_supplemental_geometry`. Written as ordinary shapes,
  not again as constructive geometry.
- **A unit's value typed (dwin's SFA, 2026-11-23).** Written in inches, every file's
  `CONVERSION_BASED_UNIT('INCH')` gave its 25.4 untyped -- OCCT named the value
  'LENGTH_UNIT', which its measure member does not know -- and SFA stopped on it ("Could not
  determine type of select value", and no part geometry). Now `LENGTH_MEASURE(25.4)`, as
  NIST's files have it; foot, mil and the others the same.
  `test_in_the_files_length_unit`.
- **SFA p6 (dwin, 2026-11-23), and what it found:**
  - *FTC-06 lost four dimensions*, its reference (1.106) and (dia1.000): they are on a curve
    compound beside the solid, which since K has no set of its own, and the SFA script
    exported the sets' parts only. Exporting the imported App::Part -- what a user does --
    then wrote no solid at all: headless, a group takes what a child links to for what it
    claims (`GeoFeatureGroupExtension::buildExport`), and the set's Part and the features'
    faces claimed the solid. A link can now say it only refers, `LinkReferOnly`
    (`PropertyLinkBase::setReferOnly`): the set's Part, a feature's References, a view's
    Objects. Their scope stays: the set's Part is Hidden, no dependency, which a Body
    needs (a Global link made the Body depend on its set). The tests' round trip exports
    the App::Part, as the script now does.
  - *An occurrence without a name*: a label whose words are a counterbore sign alone,
    stripped to ASCII, was ''. Names now give what is not ASCII as `\X2\`.
  - *CTC-05's regions with no geometry*: named 'linear dimension' as NIST names them (I),
    and associated with an empty shape aspect, where NIST associates them with nothing. A
    graphic on nothing, with no words, is now written alone, and read back into its views
    by the callout itself (OCCT).
  - *An empty semantic text*: a note's words were written a text a line, its empty line an
    empty text. Now one text, its lines broken as NIST's are (`\X2\000A\X0\`).
  - *FTC-10's fit lost* ("dia3.50 G6"): its class of tolerance is `LIMITS_AND_FITS('G6',
    'hole','','')`, the grade in the variance, which OCCT read as no variance and IT01;
    and OCCT wrote IT6 as grade 8. Both fixed; `Dimension.Fit` is read and written.
    `test_a_fit_as_the_file_gives_it`.
  - *CTC-04's [75] and [dia10]*: basic dimensions, a 'dimensional note' that is
    'theoretical'. Read into `Basic`, written so. `test_basic_as_the_file_gives_it`.
  - Not ours: SFA's nX is its own count of a size dimension's cylinder faces, and ours
    and NIST's rows agree. FTC-08-e4-tg (not in this box's NIST set) crashes SFA's viewer
    converter; to see in p7. Every round trip is checked for the first four
    (`assertWrittenClean`).
- **SFA p7 (dwin, 2026-11-23)**: CTC-04, FTC-10 and STC-10 now equal NIST's original
  (45, 32, 32 exact, none partial); CTC-05 5 -> 19 of 27, FTC-06 24 -> 27 of 34; the
  empty semantic text, CTC-05's missing geometry and FTC-08-e4-tg's converter crash gone.
  Two left, fixed:
  - *A callout in no annotation plane* (11 files): a graphic written alone (p6) returned
    before its plane. Now only its associations are skipped (OCCT).
  - *A name all `\X2\`*: SFA reads `'\X2\2334\X0\'` as no name. A graphic is named in
    ASCII: a note by its words, as before, or by its label when they have no letter or
    digit ('Text.12', as NIST names it), else by the object's name. The semantic text
    keeps its `\X2\`, which SFA reads. Both checked in `assertWrittenClean`.
  - The round trip's temporary files are removed: left behind they filled /tmp (3.7 GB),
    where the writer then could not open its file.
- **SFA p8 (dwin, 2026-11-23)**: the annotation planes took in all 11 files, the counts as
  p7's. The names did not on Windows: `std::isalnum` follows the locale, cp1252 there, where
  a UTF-8 byte of the counterbore sign (E2 8C B4) is a letter. The export and import test
  ASCII themselves (`Step::isAsciiAlnum` and kin), whatever the locale.
  `test_names_whatever_the_locale` runs where a Latin-1 locale is installed (Windows; not
  this box).
- **SFA p9 (dwin, 2026-11-23)**: every write syntax-clean, the exact counts as p8; FTC-06's
  doubled graphics gone, its notes named by their words; STC-10's coverage as the original's;
  SFA no longer misses names in CTC-04 and FTC-07. CTC-03 lost two graphics (sec 5.3 item 15,
  fixed). Nine tests named NIST editions the Windows box has newer (`ctc_04 -e1` there `-e2`,
  `ftc_08 -e1-tg` `-e4-tg` ...): a file is now found whatever its edition (`edition()` in
  `TestStep.py`). `test_names_whatever_the_locale` ran there under cp1252 and passed. SFA's
  "Processing Unicode characters" on a note's `\X2\000A\X0\` line breaks is an information,
  not an error: kept, a note's words one text.

### 10.26 A part's supplemental geometry, and one product of several shapes (2026-11-24)

NIST's CTC-04, FTC-06, FTC-10 and STC-08 carry reference and construction geometry beside the
part -- `constructive_geometry_representation`s ('supplemental geometry') of planes,
cylindrical surfaces, axis systems and sets of curves, related to the part's shape
representation. It defines nothing of the part; GD&T is on a thread's curves (CTC-04's
position and size of its thread, FTC-10's three), nothing on the rest. And CTC-04 and FTC-10
are one product of several shape representations (a solid, a surface model, a wireframe).
Read, each of the four came in as an App::Part of an object per representation, the curves
one more, and was written again as an assembly of as many products; the planes, cylinders and
axis systems were not read at all. SFA (dwin, from pass 14) lists supplemental geometry in
the files' contents, in two sheets and in its viewer, none of them a count the coverage
compares; it took our curves for none.

- **Read.** OCCT's reader keeps each representation as a group of its part
  (`XCAFDoc_DimTolTool::AddSupplementalGeometry`, `AddSupplementalItem`: kind, name, axis
  system, a cylinder's radius, a set's shape), and marks the label of a product whose
  components are its representations (`XCAFDoc::ProductRepresentationsGUID`). The PMI import
  makes each group an `App::DocumentObjectGroup` of the part, marked
  `SupplementalGeometry`: a plane a `Part::DatumPlane`, an axis system a
  `Part::LocalCoordinateSystem`, a cylinder a `Part::Feature` of its face as long as the part
  reaches along it, a set of curves a `Part::Feature` of them; each named as the file names
  it (`StepName`; a label must be unique, a name need not), the group's relationship name kept
  (`StepRelationName`). Planes, axis systems and cylinders are hidden. The curves are taken out
  of the object the reader put them in (removed when they were all it held), and the GD&T on
  them refers to the object of their set; what is left keeps its references, by their new
  indices. The App::Part of a product of several representations is marked
  `ExportAsOneShape`.
- **Written.** `ExportOCAF2` leaves a group marked `SupplementalGeometry` out of the shapes,
  and writes an App::Part marked `ExportAsOneShape` as one shape: one product, its children
  each a part of its shape, not an assembly of them. The PMI export gives each group to the
  XCAF document again, in the part's coordinates; OCCT's writer writes it as a
  `constructive_geometry_representation` of the items NIST has (`plane`, `cylindrical_surface`,
  `axis2_placement_3d`, `geometric_curve_set`, named), related to the part's shape
  representation, and GD&T on a set of curves as NIST does: a usage of the set, in that
  representation. The four files write NIST's representations item for item, and one product
  each (`TestNistSet.test_supplemental_geometry_as_the_file_has_it`).
- **Not the same yet.** The part's shapes are one `shape_representation` of all its items,
  not one per kind related to it, as NIST's are. A thread's curves shared by a tolerance and
  a dimension (CTC-04, FTC-10) are used twice, as two shape aspects; NIST's two share one, as
  for any feature two PMI are on. FTC-10's second product (ASME Y14.41, the standard it cites)
  is not written. Read without its PMI, a file has no supplemental geometry, as before.
- **NIST's February 2026 set (dwin, p16).** The suites now read the set SFA ships, newer for
  CTC-01, CTC-04, FTC-08 -e4-tg, FTC-11 and STC-09. STC-09 -e4 gives a hole two hidden 'label'
  graphics linked to a `basic_round_hole_occurrence`, an entity OCCT does not know: their
  association has no definition it reads, and both were lost. A graphic of an unknown
  definition is a graphic alone, read as a note, and written again. (The file hides 75 graphics
  by `invisibility`; none is written hidden -- a gap of its own.)
- **SFA p16 (dwin).** Graphic PMI coverage equals NIST's in every file but STC-09's labels
  (fixed above); supplemental geometry item for item; exact matches unchanged. Three follow-ups:
  a linear dimension that measures 0 -- FTC-09's .03 between two arcs of one circle, CTC-05's
  an edge to itself, FTC-06's two between faces of one plane: features read that are not what
  the file measures -- took 0 for its value and was written so (SFA: one more 'No match'); it
  is Invalid now ("its features are at no distance from each other"), keeping the file's
  value, and written with it. A `model_geometric_view` is named as its view (SFA takes one of
  no name for a syntax error, NIST's own files' too). And each supplemental item keeps the
  colour the file draws it in (`StepColor`; NIST's white or black), written as its style.
- **SFA p17 (dwin).** Exact matches up (CTC-05 23, FTC-06 33, FTC-09 27), graphic coverage
  equal to NIST's in every file. Two faults, one of them since p16: an App::Part written as one
  shape lost its children's colours -- each child's are given to its sub-shape now, as its own
  label had them, and the round trip checks each solid and surface model NIST colours is
  coloured (`assertPartsStyled`); and a supplemental plane's style held two styles (a surface
  style and a curve style), which SFA's reading of a supplemental colour cannot take ("Error
  getting color"): one style now, the surface's, its boundary in the curve's colour, as NIST's
  (OCCT cannot write the parameter lines NIST's have too). Each curve and point of a set of
  supplemental curves is styled as well, where SFA takes their colour from. A tessellated part's
  supplemental geometry (FTC-08 -e4-tg) is written; its tessellated constructive geometry (one
  triangulated face) is not read.
- **SFA p18 (dwin).** Part styles as at p14, no colour error left. FTC-10's thread curves gave
  SFA no colour: it colours a trimmed curve by its basis curve and any other member by itself,
  so those are what is styled now. STC-09's 163 face colours were lost with one shape: a
  child's elements' colours are given to the shape's own sub-shapes (XCAF takes none of a
  sub-shape's).
- **What a file hides (user decision 2026-11-25: keep it, hidden).** STEP's `invisibility`
  lists what is not shown: STC-08 its surface beside the solid (both its styles, the shell's
  yellow and its face's black half transparent), its wireframe and 29 PMI graphics; CTC-02 its
  two surfaces and 32 graphics; STC-09 40 graphics. Read, every one was shown -- the surface in
  its hidden face's black. Now an object every face of which (every edge, of one with none) is
  hidden is hidden (`ImportOCAF2::hideInvisible`), and a PMI whose graphic is listed is hidden
  (`XCAFDoc_DimTolTool::SetPresentationHidden`, `XCAFDoc::GDTHiddenGUID`). Written, a hidden
  part of a one-shape App::Part has its styles listed by an invisibility (OCCT's writer listed
  only a label hidden whole), and a hidden PMI its graphic's occurrences: NIST's counts
  (`test_what_the_file_hides_is_hidden`). SFA ignores invisibility (dwin): its counts are as
  they were, its transparency note stays. A container in a one-shape App::Part (CTC-02's group
  of its two surfaces) is one shape with it: CTC-02 is one product now, as NIST's. Open: part
  of one object hidden (STC-09's shells, in one compound with others) is shown.
- **SFA p19 (dwin).** Every SFA message equal to NIST's in all 17 files; invisibility moves
  no count. The one difference left, FTC-09's 'contains Assemblies': NIST's file is four free
  products, no assembly; read, they were an App::Part, written an assembly of them. The
  container an import makes of several free shapes is marked `ExportAsFreeShapes` and written
  as those free shapes again (`ExportOCAF2::FreeShapesProperty`); the round trip checks the
  file's assembly usages are as many (`assertSameAssembly`). Not done, SFA showing none of it:
  NIST hides more than PMI graphics and parts' styles (whole layers, FTC-09's 44 callouts,
  STC-06's placeholders); a part with per-face colours keeps FreeCAD's default colour for
  itself (STC-09's brep and shells); the products of documents a file cites (FTC-10's
  'ASME Y14.41') are not read.
- **SFA p20 (dwin): the contains line.** Item by item it equals NIST's in 13 files. 'Properties'
  (CTC-03, CTC-05, FTC-08 -e4-tg; user decision 2026-11-25: do it): a product's user defined
  attributes, read by OCCT into XCAF named data, are kept on its object (`StepAttributes`, and
  `StepIntegerAttributes`, `StepRealAttributes` for numbers) and given back on export; a part
  the file gave geometric validation properties is marked `StepValidation` and written with
  them, computed of the shape written (OCCT names them its way: 'surface area measure', one
  property each). Not written yet: the attribute validation property (counts of them), PMI
  validation properties (each graphic's length, area, facets; NIST's of its own graphics) and
  tessellated validation properties.
- **Graphics as curves (CTC-05).** NIST draws CTC-05's PMI as annotation curve occurrences
  of polylines and circles, ours were always tessellated (SFA: 'Graphic PMI (polyline)'). A
  PMI whose graphic was curves is marked (`XCAFDoc_DimTolTool::SetPresentationCurves`,
  `Callout.ImportedAsCurves`) and, drawn as the file drew it, written as curves again: a full
  circle a circle, the rest polylines, a segment chained to the one before it where it starts
  at its end (650 polylines to NIST's 556). The round trip checks as many curve occurrences.
- **Holes (STC-09; user decision 2026-11-25).** AP242 ed3 hole features -- STC-09 -e4's 8
  `basic_round_hole` definitions (name, depth and diameter each with a tolerance, a placement,
  through or not) of 25 occurrences, each identified with its faces and placed by a mapped item
  among the part's 'Positions of Holes' -- are entities OCCT does not know. OCCT's usage
  definition select takes a hole occurrence now; the reader keeps each definition and its
  occurrences in XCAF (`XCAFDoc_DimTolTool::AddHole`, `AddHoleOccurrence`; lengths in the units
  of the representation they are in), the import makes each occurrence a `PMI::Hole` of the
  set on a feature of its faces (`OccurrenceName` the file's, which need not be unique), and
  the export and writer give them back as NIST has them. A usage of several items is written
  as the `set_representation_item` it is (OCCT wrote a bare list).
- **UUIDs (STC-09; user decision 2026-11-25: product, PMI and holes, not faces and edges).**
  AP242 ed4 `uuid_attribute`s (STC-09's 615 `v5_uuid_attribute`, about 630 of them on faces and
  edges): the reader keeps the UUID of each GD&T label, product and hole occurrence
  (`XCAFDoc_DimTolTool::SetUUID`), the import on its object (`StepUUID`: type and UUID), the
  export and writer give it back, one attribute each (SFA: two on one item, or one UUID
  twice, are errors). STC-09: 79 read, 79 written, read back the same (`test_uuids_kept`).
  The faces' and edges' are not kept: an element's identity does not survive an edit.
- **SFA p21 (dwin).** The contains line equals NIST's in 15 of 17 files (CTC-03, CTC-05,
  FTC-08 -e4-tg 'Properties'; CTC-05 'Graphic PMI (polyline)'). New: a product's semantic
  text, read by OCCT as a user attribute too, was written as one as well -- invalid to SFA
  (representation named 'semantic text', 5 files): no attribute is kept of it, the note it is
  is written. Logged: STC-06's 130 length attributes come back as reals (OCCT's metadata
  drops a measure's kind); CTC-05's polylines carry 4.4 times NIST's points.
- **Gaps chased after p21 (dwin down; the user: chase them all).** Length attributes: the
  reader keeps which reals were lengths (`XCAFDoc::AttributeMeasuresGUID`, pairs of key and
  measure), the import on the object (`StepAttributeMeasures`, the real in mm), the writer gives
  them back as `LENGTH_MEASURE`s in the file's length unit (STC-06: 130 of 130). OCCT's
  `StepRepr_PropertyDefinition::SetDescription` set no flag: every attribute had been written
  with description `$`, not 'user defined attribute' -- fixed. Polylines: the export cut the
  file's curves into segments and the writer chained them (CTC-05: 4.7 times NIST's points);
  a graphic given as curves goes to the writer as it is, a polyline read (a degree 1 B-spline)
  written with its own points, only segments chained: 573 polylines, 6965 points, 77 circles,
  NIST's. Logged: NIST overrides the colour of two lines of each of CTC-05's rectangular
  regions (an over-riding styled item of a curve set of them); not read.
  What NIST hides: FTC-09 hides 44 PMI by their callouts (an INVISIBILITY of the draughting
  callout, which OCCT's invisible item select did not take: read as nothing), STC-06 the
  placeholders of 19 hidden PMI (a placeholder: no class, an undefined entity; the select takes
  both now). Read hidden, kept how (`XCAFDoc_DimTolTool::SetPresentationHiddenCallout`,
  `SetPlaceholderHidden`; `Callout.ImportedHiddenCallout`, 'hidden' in `ImportedPlaceholder`;
  STC-08 and CTC-02 hide their PMI's graphics, not their placeholders) and written so again.
  Not taken: the styles NIST gives the surfaces under faces and hides (CTC-01's 117, a styled
  item of a bare plane or cylinder: FreeCAD has no object of them); one INVISIBILITY written
  where NIST writes one an item (FTC-07, FTC-10, STC-06, STC-09: the same items hidden).
  Supplemental geometry on layers: NIST puts items on a layer each (a presentation layer
  assignment of one item, '-1' 'Layer-1') and hides most by it (FTC-07 13, STC-06 16, CTC-04,
  FTC-10, STC-09), some by their style. The reader keeps each item's layer
  (`XCAFDoc_DimTolTool::SetSupplementalLayer`) and whether it is hidden; the import puts them
  on the object (`StepLayer`: name, description; `StepHidden`), a set of curves not shown; the
  writer gives each its layer again and hides it as it was hidden: kind, name, layer and
  hidden, item for item, NIST's in all seven files (`test_supplemental_layers_...`). The
  part's own sketches and surfaces NIST hides member by member (STC-09's 5 shells and 29
  curves, FTC-10's curves; one object in FreeCAD hides no part of itself; user decision
  2026-11-26: the assembly route): a compound some members of which the file hides, not all,
  is read as an object each in an App::Part written as one shape again
  (`ExportOCAF2::OneShapeProperty`), those hidden hidden, each with its own faces' and edges'
  looks (ImportOCAF2 `hiddenMembers`; the progressive import hands such a file to the direct
  one). The PMI on them refers to the members it is on, the first its object, the others
  beside it; the part of a member without a solid is the one its container is in. Written,
  the members hidden are NIST's, curves and surface models (`test_what_the_file_hides_is_hidden`,
  now on FTC-10 and CTC-04 too; NIST styles a hidden curve itself, OCCT the set it is in). Not
  done: members of a curve set hidden alone (STC-06's 12, FTC-10's two axis lines), each a
  supplemental item; STC-09 -e3's hidden points (curve sets of a point each) are read hidden
  but not written so -- OCCT writes a part's points as one set.
  Which supplemental items a saved view shows: STC-06's twelve views each map, beside the
  part, a shape representation 'supplemental geometry' of some of the items, described
  'supplemental geometry subset' (a description attribute), as a mapped item of the camera
  model. The reader took its items for shapes; it now finds them among the items read
  (`XCAFDoc_DimTolTool::SetViewSupplemental`, a reference list on the view), the import lists
  them in the view's `Objects` beside the part -- so the rest is not shown in it -- once the
  supplemental geometry is read, and the writer writes the subset again, each item the one it
  wrote for the group: kind and name, view for view, NIST's
  (`test_view_supplemental_geometry_as_the_file_has_it`).
  How a PMI's shapes are grouped: NIST gives a pattern of holes as a composite group shape
  aspect 'pattern of features', named as the annotation, of a composite shape aspect 'unified
  geometric item' per hole (CTC-04's 'Position.2': 6 holes of 2 faces), 169 groups in 10
  files, one shared by the position and the size of its holes; we wrote one flat composite,
  unnamed. The reader keeps the tree (`XCAFDoc_DimTolTool::SetAspectStructure`: kinds, names,
  descriptions, each leaf its shapes' label entries, the composite's entity number to say
  which PMI share it), the import as the elements of the feature's objects (`StepAspects`,
  read once supplemental geometry is objects of its own: a thread's cylinder is
  `Curves001.Edge1`), the export as labels again, and the writer writes it so -- one
  composite to PMI that shared one, if it still covers each one's shapes, else flat as
  before. A PMI on a datum's feature itself (FTC-07's sizes on datum B's, STC-06's flatness
  on D's) is on the feature written for that datum again, named as the file named it; a
  datum's feature is written as the file grouped it (STC-09's datum E: a group of a hole's two
  faces and a hole occurrence; a group, 'F', where the file's is one). A hole occurrence is
  an entity OCCT now knows (`StepRepr_BasicRoundHoleOccurrence`): before, its faces were lost
  from the PMI on it. Shapes of the part beside its solid -- FTC-10's extracted surfaces,
  STC-10's wires -- stay in the feature, after the solid's, where they were dropped. A
  centre of symmetry (STC-07's 23 axes of its holes, 'S') is written again, derived from the
  surfaces it is of, its own axis the curve of the supplemental geometry it was (named among
  the supplemental objects, `Curves016.Edge1`); a size that is its own datum feature (STC-07's
  datums B and C, 'W') is one dimensional_size_with_datum_feature again, not a size on a
  datum feature; a leaf of several faces is one shape aspect of them all (an
  item_identified_representation_usage of a set). All sixteen files as NIST has them
  (`assertSameAspects`, in the every-file round trip). Not kept: a datum feature's own form
  where no PMI is on it -- its faces on its own usage or on members, the geometry on the
  datum and not its feature (CTC-02); SFA does not report it. Found on the way: a
  feature named a face and an edge lost its edge, an edge taken for a tessellated copy of the
  face (FTC-11's arc length).
  Cited documents: the standards a file says its PMI follows (FTC-10 'ASME Y14.41', STC-07's
  four; CTC-05, FTC-07, STC-09 too) -- a product of its own with a DOCUMENT, a document
  product equivalence and an applied document reference to the part's definition with a
  role, under a context association. SFA reads them, unlike the rest of this list: its
  'Standards:' line and the standard rows of its coverage come from their ids. The reader
  keeps each, field by field (`XCAFDoc::CitedDocumentsGUID`, one string of '\n'-joined
  fields per document: context, document, role, the equivalent product), the import on the
  object (`StepDocuments`), the writer writes them again: entity for entity NIST's in the
  five files (`test_cited_documents_as_the_file_has_them`).
  A part coloured face by face: the import gave it its faces' colours and kept the default
  grey as its own, which no face has, so every face differed from it and was written as a
  colour of its own (STC-09: 163 where NIST has a body colour and 5). Its own colour is now
  the one most of its area has, as for materials -- and the looks of a face given a colour
  copied from the default look lose its material card: two looks naming one card compare
  equal whatever their colours, which had made the 163 one. The other files' face styles
  NIST writes and we do not are each face of a solid styled as the solid is, or a face of a
  surface model as its model is (`assertBodyColoured`). Not done: the five surfaces of
  STC-09 are written in the body's colour, their face each in its own (drawn the same).

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
| 29794 query a feature's specifications | P1 query tool (10.8) |
| 29795 TechDraw pages from views | 9 + P4 |
| 29796 STEP AP242 import | 5.1 -- P2 (semantics read 10.9; graphics, notes, views 10.10) |
| 29797 STEP AP242 export | 5.2 -- P3 |
| 29798 semantic access for other workbenches | the Python API: every object is a plain document object with typed properties, P0 |

What this design adds over the proposal: the shared toleranced feature (4.2, STEP's
`shape_aspect`), the generated graphic from P1 rather than as a later goal (6.1), repair built
into recompute (7), ASME beside ISO (4.1), and the OCCT fork work it needs (5.3).
