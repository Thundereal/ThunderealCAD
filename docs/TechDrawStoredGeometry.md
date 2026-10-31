# TechDraw stores what it projected

Status: built, 2026-09-07, on `LinkVibe`.

Until now a TechDraw view kept nothing of what it drew.  The projected
edges, vertices and faces -- and a section's cut faces -- lived in
`GeometryObject`, a transient member, and the only way to get them back
was to make them again.  So every time a drawing was reopened,
`DrawPage::onDocumentRestored()` called `updateAllViews()`, which called
`recomputeFeature()` on every `DrawViewPart` on the page, which ran the
boolean cut (for a section), HLR, and face finding, to arrive at exactly
the picture that had been on the screen when the file was written.

This document is the change that stops that: **the projection is written
into the document, and a restored page draws it instead of rebuilding
it.**

## 1. What was there before, and what was not

- `DrawViewPart::m_geometryObject` -- transient.  Edges, vertices,
  faces, all rebuilt on every recompute.
- `DrawViewSection::m_cutShape`, `m_cutShapeRaw`, `m_cutPieces`,
  `m_preparedShape`, `m_sectionTopoDSFaces`, `m_tdSectionFaces` -- all
  transient, all rebuilt (`docs/TechDrawPortAndSection.md` sec 31.1 says
  so outright: "Transient, like the shapes: they rebuild on
  recompute/restore").
- `DrawViewDimension::SavedGeometry` (`Part::PropertyTopoShapeList`) --
  the one thing a view already stored, and it stores a *dimension's*
  reference geometry, not the projection.
- `DrawViewPart::UnderlayImage` -- the shaded raster underlay is
  persisted (sec 26 of the port doc), which means the *picture* survived
  a reload but the geometry under it did not.

So: no, TechDraw did not store its section geometry, or any other
projected geometry.  It stored a raster of it.

## 2. What is stored now

One property on `DrawViewPart`, inherited by every view that projects --
section, detail, projection-group item, broken view:

    ProjectedGeometry   TechDraw::PropertyProjectedGeometry
                        Prop_Output | Prop_Hidden

`Prop_Output` for the reason `UnderlayImage` has it: the value is
derived state written back by the thing that produced it, and writing it
must not touch the view, or every projection would schedule the next
one.

The property holds

- the **projected edges**, in the order the view numbers them, each as
  the `TopoDS_Edge` it carries plus the attributes `BaseGeom` keeps
  beside it: geometry type, class of edge, HLR visibility, reversed,
  `ref3D`, the source element name and the projected edge's own name
  (`docs/TopoNamingEnhance.md` sec 8.5), cosmetic flag, source and
  source index, cosmetic tag;
- the **projected vertices**, in order: point, visibility, centre-mark
  and reference flags, cosmetic tag, the source names and the vertex's
  own name (sec 8.6.1);
- the **projected faces**, in order: the edges of each bounding wire,
  flattened, with the count per wire, plus the face's source names and
  its own name;
- the **centroid** the projection was made against
  (`DrawViewPart::getOriginalCentroid`), which nothing else stores;
- the **cut faces** of a section (`m_sectionTopoDSFaces`), which is what
  the cut surface is drawn and hatched from.  Empty for every other kind
  of view.

The curves travel as a BRep file of the property's own -- one compound
of three: the edges, the face-wire edges, the cut faces -- because that
is the only form that round trips a projection exactly.
`BaseGeom::baseFactory()` reads an edge back into the same subclass with
the same derived values HLR gave it, which is the same call the
projection itself makes.  Everything an edge does not carry is an XML
record beside it, one per element **in the view's own order**: that is
what keeps `Edge3` the same `Edge3` after a reload, which the whole of
the naming work (T3) depends on.

### 2.1 Where it is captured

Where the geometry is complete, and nowhere else:

- `DrawViewPart::onFacesFinished`, after `postFaceExtractionTasks()` --
  so the capture includes the cosmetic vertexes, cosmetic edges and
  format names `postHlrTasks` added, and the centre lines that had to
  wait for faces;
- `DrawViewPart::onHlrFinished`, in the branch where no face finding
  will run at all (`CoarseView`, or faces switched off).

`DrawViewSection` overrides `captureGeometry()` to put
`m_sectionTopoDSFaces` in beside the projection.

### 2.2 Where it is put back

`DrawViewPart::onDocumentRestored()` builds a `GeometryObject` from the
property and installs it, restores the centroid and the bounding box.
`DrawViewSection` additionally rebuilds `m_tdSectionFaces` from the
stored cut faces with `makeTDSectionFaces` -- a face walk instead of a
boolean.

The restore refuses rather than guesses.  If the stored edge count does
not match the record count, or an edge does not read back through
`baseFactory`, nothing is installed and the view projects the way it
always did.  A stored format newer than this build reads is skipped the
same way.

## 3. Losing the rebuild on restore

`DrawPage::onDocumentRestored()` called `updateAllViews()`
unconditionally (under `canUpdate()`).  It now calls
`updateAllViews(true)`, and the first loop -- the one that recomputes
every `DrawViewPart` -- skips a view when `canReuseStoredGeometry()`:

    m_geometryFromStore && !m_restoredOutOfDate && !isTouched()

The second loop is unchanged: dimensions, balloons, hatches, leader
lines and the rest are still recomputed, because what they derive
(measurement points, hatch line sets) is transient and cheap, and it is
read off the restored geometry.

**`m_restoredOutOfDate` is read in the view's own `onDocumentRestored`,
not in the page's.**  `Document::afterRestore` walks objects in
dependency order and purges the touched flag as it goes, so by the time
the page is reached its views have already lost the "I was written out
of date" mark that the file carried (`Touched="1"`,
`Document.cpp:1918`).  The view reads it first, because the page depends
on its views and is therefore restored after them.

### 3.1 The other view that projected on restore

`DrawProjGroupItem::onDocumentRestored()` did not go through the page at
all: it called `DrawView::onDocumentRestored()` -- skipping
`DrawViewPart`'s -- and then ran its own `execute()` outright, so every
item of every projection group re-projected on restore whatever the page
decided.  It now calls `DrawViewPart::onDocumentRestored()` and returns
when the projection came back with the document; the `execute()` is the
fallback for when it did not.

### 3.2 The switch

`TechDraw/General/StoreProjectedGeometry` (default **on**),
`Preferences::storeProjectedGeometry()`.  Off, a view stores nothing and
a restored page rebuilds exactly as it always did -- which is also how
the before-and-after in section 4 is measured in one build.  A document
written with it on and opened with it off simply reprojects and then
overwrites what it stored.

### 3.3 The dimensions still have to be told

`updateAllViews()` has always had two loops: the part views, which
project, and then everything derived from them -- dimensions, balloons,
hatches, leader lines -- which is recomputed with `overrideKeepUpdated`.
The second loop is now `updateDerivedViews()`, and the restore runs it
even when the page may **not** update, provided some view brought a
projection back.  What those views hold is transient and cheap, and it
is read off the projected geometry, not off the model -- so this is not
the page following the model, it is the restore finishing.  Without it,
a page with `KeepUpdated` off comes back drawn (which is new) with every
dimension reading zero (which would be new and wrong).

## 4. Measured (2026-09-07)

Two probes, both under the GUI harness (`run_cdb.ps1 -StartupScript`,
`docs/TopoNamingEnhance.md` sec 8.0), scratchpad scripts in keeping with
the probe convention.

**It comes back element for element.**  A 40x30x20 box with a 12mm hole
through it, a base view, a section through the hole, and a dimension on
`Edge0`:

    built   view (5 edges, 7 vertices, 2 faces), section (10, 8, 4)
    saved   31754 bytes, closed, reopened
    at the instant open() returned, before any thread could run:
            the view already had its (5, 7, 2)
    reload  the same counts, the same six element names, the same
            vertex and face names, the dimension still 30.000, the
            section still 2 cut faces
    verdict identical = True, for the view and for the section

**And it comes back where nothing is allowed to project.**  The same
document with `KeepUpdated` off, which is the page saying it may not
follow the model: the restore recomputes nothing, so geometry that is
there can only have come out of the file.  Identical again, cut faces
included.  (This is also the case that made
`updateDerivedViews()` necessary -- see 3.3.)

**What it costs, and what it saves.**  A 200x120x20 plate with 84 holes,
a base view (88 edges, 256 vertices, 85 faces) and a section (40, 28,
14), measured twice in one build by flipping the preference of 3.2 --
which makes the second run exactly the old behaviour:

                          stored          not stored
    file                  94736 bytes     76338 bytes   (+18398, +24%)
    open() returns        0.282 s         0.249 s
    page fully drawn      0.001 s         0.542 s

The last row is the point: with the projection stored, the page is drawn
when `openDocument` returns.  Without it, the cut, HLR and face finding
run again after the open and the page fills in half a second later --
for a model that takes 4.7 s to build from scratch.  The file cost is
about 35 compressed bytes per stored element.

**And it draws the same drawing.**  The counts and the names are one
thing; what reaches the paper is another.  The same page, exported with
`TechDrawGui.exportPageAsSvg` straight off the stored projection and
again after being forced to project the whole page: **the same 290313
bytes, the same 705 path elements, the same path data**.  The two files
are not byte-identical -- the vertex dots come out in a different order
within the scene, because a rebuild recreates those items -- and that
raised the obvious question, which was then asked of the geometry
instead of the paper:

    from the store           256 verts, 88 visible edges
    projected again          256 verts, 88 visible edges
    view    same vertex order = True, same edge order = True
    section same vertex order = True
    projection vs projection: same order = True

So the numbering a stored projection comes back with is the numbering a
fresh projection of the same shape produces, element for element; the
SVG difference is the exporter's scene order and nothing else.

## 5. What this does not do

- **The 3D shapes are still transient.**  A section's `m_cutShape`,
  `m_cutPieces` and `m_preparedShape`, and a detail's `m_detailShape`,
  are not stored.  Nothing that *draws* needs them; the first real
  recompute rebuilds them as before.  A detail view on a section draws
  from its own stored projection, so it does not need its parent's cut
  either.
- **Nothing is deduplicated across views.**  Two identical views store
  two copies.
- **The stored projection is never consulted by a recompute.**  It is
  read when the document is opened and, since sec 6, when an undo or a
  redo puts it back; any recompute overwrites it.

## 6. An undo puts the projection back (user, 2026-10-31)

Found by `tests/gui/td-model-interleave.py` and `docs/MultiViewEdit.md`
sec 12.10: after an undo of a change to the model, the drawing showed the
model from before the undo. The user's ruling: an object must not keep
what it shows outside its properties -- that is a design error, and it is
corrected here, not worked around.

### 6.1 What was kept where

Measured on a box drawn by a part view, its length changed from 15 to 20,
recomputed, undone, redone; after each step a copy of the document was
reopened with the page not updating, which draws from `ProjectedGeometry`
alone:

    after              model   view draws   ProjectedGeometry holds
    build              15      15           15
    edit + recompute   20      20           20
    undo               15      20           15
    redo               20      20           20

Log on and log off alike. The projection was in a property all along, and
the undo put it back; the view read it at a file's open and never again.
What TechDraw kept outside its properties came in four kinds:

- **What a view draws** -- the projection, in `m_geometryObject`. Stored
  (sec 2); not read back after an undo.
- **What is measured off it** -- a dimension's points (`m_linearPoints`,
  `m_arcPoints`, `m_anglePoints`), which its value is read from. Made by
  `execute()` and nothing else. (Hatch line sets look like this and are
  not: `m_lineSets` holds the pattern read from its file, a function of the
  hatch's own properties, and the lines are trimmed to the face each time
  they are read.)
- **The 3D shapes a view keeps for the views made from it** -- a section's
  cut (`m_cutShape`, `m_cutShapeRaw`, `m_preparedShape`, the frames), a
  detail's piece (`m_detailShape`), a complex section's aligned shape.
  Never drawn; empty after an open, and after an undo still the shapes of
  the state the undo left.
- **Machinery** -- watchers, progress, waiting flags, held errors. Not
  state, and left alone.

### 6.2 What the log pays for the store

The plate of sec 4 (200x120x20, 84 holes), a base view and a section,
precise HLR under the GUI harness, the height changed three times with a
recompute after each, measured with the store on and with it off
(scratchpad `logcost_probe.py`):

    per step                    store on          store off
    values added                8-9               5
    their raw size              440-522 KB        172 KB
    the log's growth            5.6-20 KB         1.5-1.8 KB
    recompute, cut and project  0.46-0.73 s       0.48-0.56 s

The two `ProjectedGeometry` values of a step are 91 KB and 42 KB raw, 4.0
and 3.0 KB in the log, and the BRep beside them -- the curves, an
attachment of the value -- 133 KB raw and 8 KB in the log; the values of
the step before are rewritten as reverse deltas against them, 31 to 360
bytes each (`docs/TransactionLogJournal.md` sec 23.2). Net, on this page,
**the store costs the log 4 to 8.5 KB a recompute, 19 KB on the first**,
three to six times what the step costs without it.

They are logged as input, not derived (the op's `derived` is 0, the tier
`durable`): the projection is written when its thread is done, after the
recompute has returned, in an `<implicit script>` step of its own -- one
that rides with the user step before it, which is what makes an undo take
it back. A derived value would be evictable under the cache budget (sec 10
of the journal); this one stays as long as the log. Whether a value an
object writes into its own `Prop_Output` property outside a recompute
should be logged as derived is a question for the log, left open.

### 6.3 An undo draws what it put back

- **`DrawViewPart::onChanged`** -- `ProjectedGeometry` written while the
  document performs an undo or a redo sets `PendingTransactionUpdate`.
  (`DrawViewSection::onChanged` passed its changes to `DrawView`, past
  the part view, and so never got here; it goes through `DrawViewPart`
  now.)
- **`DrawViewPart::onUndoRedoFinished`** -- once the transaction is done:
  every thread still making the view's result is stopped
  (`abortMaking()`: the projection, a section's cut, a detail's piece, a
  complex section's alignment -- each of them was of the state the undo
  left), the stored projection is installed as at a file's open, and the
  view and what is drawn on it are repainted. Nothing stored to install
  leaves the view to recompute.
- **`DrawViewPart::resultInProperties()`** -- true while the preference
  stores the projection (`d88b86df28` made it false) and no thread is
  making one. A view caught projecting is one whose stored projection may
  be older than its inputs, and `addResultsNotKept` now also sets
  `PendingTransactionUpdate` on what it picks, so a view the undo did not
  write, but whose inputs it did, has its threads stopped too, rather than
  finishing after the undo and drawing -- and storing -- the state taken
  back.

So an undo of a step whose projection had finished leaves the view drawn
right and clean, and so does an undo made while the step's projection is
still running: what is stored is then the projection of before the step,
which is the state the undo leaves.

### 6.4 The shapes kept for the views made from a view

A section keeps its cut (`m_cutShape`, `m_cutShapeRaw`, `m_cutPieces`,
`m_preparedShape` and their frames), a detail its piece, a complex section
its aligned pieces. They are not stored: written into every recompute's
row they would cost what sec 10 of the journal refuses for `Shape`. A view
that installs a stored projection -- at a file's open or after an undo --
drops them (`dropWorkingShapes()`) and says so (`workingShapesDropped()`).
A section or detail that finds its base's shape missing for that reason
has the base recompute (`remakeWorkingShapes()`), and waits for it the way
it waits for a cut still running. Before, after an open the detail said
its base had no shape, and after an undo it used the cut of the state the
undo took back.

The detail tells the sections made from it when its piece is made, as a
section tells the views made from it when its cut is (`postSectionCutTasks`).

### 6.5 What is measured off a view

A dimension's points are worked out when they are read, and kept until
the view's geometry or a property of the dimension changes:
`GeometryObject::stamp()` changes with every change of the edges, vertices
or faces and differs between any two geometry objects. `execute()` keeps
what writes -- repairing references by name and by geometry, which writes
`References2D`, `SavedNames` and `SavedGeometry` -- and measures last;
`measure()` is the measuring alone, which the extent and landmark
dimensions have their own of. The landmark keeps writing its reference
vertices and tags in `execute()`. `DrawViewDimension::resultInProperties()`
is gone: what it shows is read off properties and the view's stored
projection.

### 6.6 A view that waited for a cut was not recomputed

Found on the way, on the base build as on this one: a detail of a section
was empty after the recompute that made it. The detail recomputes while
the section is still cutting, finds no cut, and waits;
`postSectionCutTasks` then calls its `recomputeFeature()` -- which skips
it, as nothing of it is touched (`OptimizeRecompute`,
`Document::_recomputeFeature`). It drew at its next recompute of its own,
from the cut before. The section now has it recomputed the way a view has
its balloons recomputed (`enforceRecompute()` under `NoTouch`), and so do
`remakeWorkingShapes()` and the detail's own call.

### 6.7 What this does not do

- **A projection running from an earlier recompute** when a later step,
  never recomputed, that changed the view's inputs is undone: the thread
  is stopped and the stored projection drawn, which is of before that
  earlier recompute, and nothing marks the view. Each step of an undo run
  is taken under a guard of its own, and the step that finds the view no
  longer projecting puts back the touched state the rows give it. It
  takes an undo within a projection's time of a recompute.
- **A dimension's references written back by an undo** run
  `updateSavedGeometry()` in `onChanged`, which reads the view's geometry
  before the view has installed the projection the undo put back. A write
  inside an undo, of a value from the state the undo left (journal sec
  23.6); left as it was found.
- **The switch off** (`StoreProjectedGeometry` false): nothing is stored
  and nothing is installed; `resultInProperties()` is false and an undo
  marks the view to recompute, as `d88b86df28` made it.
- **A complex section whose cut lands before its alignment** says it is no
  longer waiting while the alignment still runs; a detail of it may be
  made from nothing. Found by reading, not changed.

Tests: `TDTest/DrawViewUndoTest.py` (App, CoarseView, log on and off) and
`tests/gui/td-undo-stored-projection.py` (real HLR, a dimension, a section
and a detail, and an undo made while they project).
