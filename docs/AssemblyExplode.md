# Exploded views: calculation, showing, animation, drawings

Branch `AsmExplode` (from main 22c5630627, 2026-11-14). The exploded view
and its steps are upstream's objects, made C++ in
`docs/AssemblyObjectModel.md` sec 7.2 (`src/Mod/Assembly/App/Views.h`).
This work gives them one calculation, a way to be shown that is not an
edit, an animation, and a place in TechDraw. Dynamics and the solver are
dwin's (the Windows box); this branch stays out of them.

## 1. The plan, as agreed (2026-11-14)

| Step | What | State |
|---|---|---|
| E1 | One calculation in C++: placements and trails at a progress through the steps | built |
| E2 | Showing an explode is not an edit | built |
| E3 | An "Explode" animation track kind, its value the progress | built |
| E4 | TechDraw: an exploded view as a drawing view's source, with its trails dashed | built |

The user agreed with three recommendations:
- **One track** whose value runs 0..N, each step one unit, so easing and
  timing come from its keys; not a track per step.
- **Auto explode** (steps made from the joints and part sizes) is out of
  scope until E1..E4 are done.
- **TechDraw extras** (balloons tied to the BOM, a view per step) are out
  of the first pass.

## 2. The calculation (E1)

`ExplodedView::explodedAt(progress)` says where the steps put the parts
and the trail each left, and writes nothing.

- **Progress.** 0 is the assembly at rest and n is the end of step n. A
  fraction is part of the next step's move. Below 0 is the start, past
  the end is the end, and the default is the end.
- **A part's move at a fraction.**
  - A Normal move is a power of the move (`Base::Placement::pow`), so a
    turn about a center follows its arc and does not cut the chord.
  - A Radial move is that share of its distance.
- **Frames.**
  - A placement is the part's `Placement` value, in its parent's frame.
  - A trail is in the document's frame, where a step's view provider
    draws it.
  - A part inside an `App::Part` is placed through that Part. If a step
    moved the Part too, its frame is taken at rest.
- **Trails.** One trail per part per step started. Each runs from where
  the part's bounding-box center is when its step starts to where it is
  now. Upstream's `_calculateExplodedPlacements` started every trail at
  the unmoved place.
  - A turn's trail follows its arc: the path is sampled every 5 deg or
    less, along the powers of the move. A slide's is its two ends.
    Upstream drew a turn's trail as the chord.
  - `Trail.path` carries the points. A step's `Trail` property holds
    segments, so an arc is several; its view provider joins the segments
    that meet into one line, so the dashes run along the arc (a dash
    pattern starts again on each line).
- **Radial.**
  - A radial step moves a part away from the assembly's bounding-box
    center, by 4 x the move's length / the assembly's diagonal, as
    upstream.
  - The center and size come from the assembly's shape at rest
    (`getCenterAndSize()`). Upstream read the view provider's bounding
    box, so a console fell back to 0 and 100.
  - Radial results move a little from upstream's in the GUI: a shape's
    box is not a view provider's.
- **Steps.** `ExplodedViewStep::explode(step, frame, fraction)` applies
  one step to an `ExplodeFrame`. It reads the step by its properties, so
  upstream's Python steps in an unconverted file work too.
- **The panel** (`TaskAssemblyCreateView`) keeps its flow. Its
  `applyMoves`, `applyStep` and `_calculateExplodedPlacements` call the
  C++ for a C++ view or step, and write as before.
- **Trails on show.** A step's trails are in `Trail` (transient, never
  saved). Its view provider draws them on `updateData`.
- **Python.**
  - `ExplodedView`: `explodedAt()`, `getCenterAndSize()`, `getSteps()`,
    `getAssembly()`.
  - `ExplodedViewStep`: `explodeFrom(center, size, fraction)`.
- **Checked against upstream.** Upstream's Python, run against the same
  view, gives the same placements to 1e-9. The view had four Normal
  steps over three parts, one part inside a turned `App::Part` and one
  turned about an oblique axis.

## 3. Showing it is not an edit (E2)

**Before.** Selecting a view in the tree (with its assembly active)
called the proxy's `explodeTemporarily`, which wrote each part's
`Placement` as an edit. Measured headless:
- a transaction was left open, so there was an undo step;
- the Assembly was left touched after the restore, so the next recompute
  solved it;
- in the GUI, the document was marked modified.

**Now.** `ExplodedView::pose(progress)` and `unpose()` write under
`App::WriteScope(WriteCause::Animation)`, as an Animation plays
(`docs/Animation.md` sec 5.1).

What a pose does:
- It writes under the Animation scope: no step, no undo, not dirty, not
  modified.
- It purges whatever its writes touched.
- It shows each step (`Visibility`) and fills its `Trail`. Both are put
  back on unpose.

How it keeps the design:
- **Rest placements.** The first pose keeps each part's rest placement,
  and every pose is from rest, so poses do not compound.
- **A save** writes the rest placements: the pose comes off at
  `signalStartSave` and goes back at `signalFinishSave`.
- **A recompute** runs on the rest placements, the same way, at
  `signalBeforeRecompute` and `signalRecomputed`. Without this, an
  assembly with joints would solve from the exploded places.
- **Anyone else's write wins.** A part written by anyone else while
  posed keeps that value: an edit, an undo or a script wrote the design.
  The next pose starts from it.
- **Deleting the view** while posed puts the parts back
  (`unsetupObject`).
- **Editing a step while posed shows it again**: a step's move, parts or
  type, or the view's list of steps, re-poses at the progress on show. So
  does an undo or a redo, at its end, from the design it put back.
- **An edit while posed records the design as the value before.** A
  transaction asks `App::Animation::restValueOf()` what a property held
  before an edit, and the exploded view answers through
  `addRestProvider()`: a part's rest placement, and its shape at it (a
  part's shape follows its placement, and is recorded after it, so it is
  judged by its own placement). The undo of the edit then puts the design
  back, not the pose. Found on the way: an Animation had the same hole
  for a shape, the undo of an edit of a Part box played putting back its
  pose; `Animation.md` sec 5.2.
- **The transaction log keeps the design too.** An edit, then the view
  shown, in one transaction: the log's row is the edit, not the pose, as
  `Animation::heldRestValue()` asks the same providers (`Animation.md` sec
  5.2). The merge of main on 2026-11-15 brought that lookup for
  Animations only; the row held the pose (20, 0, 120) until then.
- **Rest boxes are kept.** Each part's box at rest is taken at the first
  pose and kept, so a part holding parts a step moved is not boxed as
  posed. It was not, before, only because such a part's shape is cached
  from its last recompute.

Who calls it:
- `ViewProviderAssembly::explodeTemporarily` and
  `clearTemporaryExplosion` call it directly for a C++ view; the proxy's
  `explodeTemporarily` and `restoreAssembly` do the same.
- The edit panel unposes every view of its assembly before it reads the
  placements, so it starts from the design and goes back to it.
- An upstream Python view in an unconverted file keeps upstream's path.

Python: `pose(progress=None)`, `unpose()`, `posedAt()`.

## 4. The animation (E3)

An Animation plays an exploded view through a track of kind `Explode`
(`docs/Animation.md` sec 3.6), registered by Assembly's App module.
- **The track.**
  - Its handler data is the view's name in the Animation's document, so
    its path is `Explode:<name>`. A label names it too.
  - Its one number is the progress: 0 to the count of steps.
  - Each frame poses the view there (sec 3), so playing is no more an
    edit than the tree's explode is.
  - Stopping the Animation unposes the view.
  - A track of a view that is gone writes nothing.
- **Keys.**
  - Keyed like any track; keying it now reads the progress on show.
  - A handler track of one number takes a bare number, an int too
    (`Animation::setKey`; it took only a list of floats before).
- **Explode, then assemble.** A strip of the explode Animation in Reverse
  plays the assemble, from the one recording.
- **Found on the way: stopping did not put back a strip's handler
  tracks.** `Animation::deactivate` called `deactivate` on the handlers
  of its own tracks only, while `evaluate()` writes those of the
  Animations it plays as strips too. A stopped storyboard left the view
  exploded, and would have left a strip's camera where it was. It now
  walks the strips the way the evaluation does, eight deep at most.
- **No Solved mark.** The parts' `Placement` is never keyed, so the
  Animation's Solved state for an assembly's placed parts (mini's
  `addPlacementSolver`) does not apply.
- **Making one.** `CommandCreateView.makeExplodeAnimation(view,
  secondsPerStep=1.0)` makes an Animation with two linear keys: 0 at the
  start, n at n seconds. The view's context menu has **Animate**, which
  makes one in a single undo step, makes it active and raises the
  timeline.
- **Exported** (2026-11-15). Both ways an Animation leaves the program
  carry an explode:
  - **streamed** to the web viewer, the clip baked from what is drawn
    (`Animation.md` sec 9.1) has a track for each part moved and the
    steps' trails as sequences, their draws at every frame. It needed
    nothing; `tests/gui/serve-explode.py` holds it;
  - **glTF** (`freecad.animationgltf`) sampled each part's placement, but
    dropped the trails, silently: a step has no placement and its lines
    are not a shape. The exporter now asks any object whose proxy has
    `gltfLines(obj)` for its lines at every frame, and the view's proxy
    answers with its steps' trails. Each shape a step's trail takes is a
    mesh of LINES, unlit in the steps' colour, on a node shown at the
    frames it is that shape (`Lines` in the result). Not dashed: glTF has
    no line pattern. `TestExplodeTrack.testGltfExport`; before, no line
    node and no `Lines`.

## 5. Drawings (E4)

**An exploded view has a shape.** `ExplodedView::getExplodedShape(progress)`
is the assembly as the steps put it, in the document's frame, writing
nothing:
- what the assembly shows, its parts moved where the steps put them; a
  part inside an `App::Part` is moved inside it, the rest of that Part
  drawn where it is;
- the trails as edges named `Trail<n>`, one edge each: a line, a turn's
  arc of circle, or, for a turn that also travels along its axis, a curve
  through the arc's points (sec 2). One edge, because TechDraw starts a
  dash pattern again on each edge: the first version, an edge per 5 deg
  piece, drew a turn's trail solid, each piece one dash.

`getSubObject` hands it to anything that asks the view for its shape, so
`Part.getShape(view)` and a TechDraw view whose Source is the exploded
view draw the explode at its end, with no TechDraw code that knows the
Assembly. The assembly's own shape does not take the view in: it exports
its components only. A re-entry guard keeps a view out of its own shape
should that change.

**Trails are drawn dashed.** A projected edge keeps the mapped name of the
3D edge it came from, and TechDraw asks the source's view provider how to
draw it (`Gui::ViewProvider::getEdgeInfo`, the hook sheet metal's flat
pattern uses for its bend lines, `docs/SheetMetal.md` sec 5.6). The
exploded view has a C++ view provider for it now,
`AssemblyGui::ViewProviderExplodedView` and its Python variant, which the
Python proxy attaches to as before. It answers for a trail's name:
dashed, in the steps' line colour (`StepLineColor`, the 3D view's).

Found on the way:
- **The 2D page engine ignored a source's edge styles.** `PageFeed` (the
  vg page, `docs/TechDrawPortAndSection.md` sec 16) drew every edge
  without a format of its own solid black. `QGIViewPart` asked the
  source's view provider. It asks now too, so sheet metal's bend lines
  are styled there as well.
- **`StepLineColor`'s last byte is not an opacity.** Coin's line colour
  reads the colour alone. Read as a `Base::Color` its default 0xCC333300
  is fully transparent, and the first version drew every trail invisible
  on both pages, though the SVG said dashed. The view provider takes the
  colour and makes it opaque.

## 6. Tests

`src/Mod/Assembly/AssemblyTests/TestExplodedView.py`:

- `TestExplodedView` (15, E1):
  - a translation; a turn about a center, half of it on the arc; two
    steps composed; one step moving two parts;
  - the same as upstream on Normal moves;
  - radial, at the end and halfway;
  - progress 0 .. past the end;
  - the assembly moved; a part in a Part;
  - a missing part skipped; an empty view;
  - a turn's trail on its arc, halfway only as far as it went; a slide's
    two points (2, added with the arc trails);
  - the panel's applyMoves and applyStep; a step's explodeFrom.
- `TestExplodedViewShown` (13, E2):
  - the tree's explode and restore are not an edit;
  - pose and re-pose from rest; the trails and the steps' visibility;
  - explodedAt while posed;
  - a part in a Part that a step also moved;
  - a save while posed writes the design; a recompute while posed solves
    the design (a revolute joint), the pose back after;
  - an edit while posed is the design;
  - deleting the posed view;
  - editing a step, its parts and the steps while posed, shown again;
  - the undo of an edit while posed back to the design; an undo while
    posed shown exploded from the design;
  - a Part holding a part moved, its rest box kept (4, with the open
    items).
- `TestExplodeTrack` (4, E3):
  - playing, at fractions and at the ends, is not an edit, and stopping
    puts the parts back;
  - a key by the view's label, and a key of the progress on show;
  - explode, then a Reverse strip of it assembles, and stopping the
    storyboard unposes the view;
  - a track of a deleted view writes nothing.
- `TestExplodedViewAnimate` (1, GUI only): the context menu's Animate
  makes the Animation in one undo step.
- `TestExplodedShape` (5, E4):
  - the shape: both parts, the moved one where the step put it, one trail
    edge named Trail1 from where to where; `Part.getShape(view)` the
    same; the design untouched; at a progress;
  - a turn's trail as one edge, Trail1, the quarter circle's length and
    radius, not the chord's;
  - the assembly's own shape without the view;
  - the shape the same while posed;
  - a TechDraw view of the exploded view: both parts, and projected edges
    whose source is Trail1 (needs an event loop: GUI only).
- `TestExplodedViewDrawn` (1, GUI only): on the page, the trail is a
  stroke in the steps' colour, dashed and opaque, in the SVG and on the vg
  page (counted in pixels), and none without the trail.
- `TestExplodedViewInTree` (1, GUI only): selecting the view with the
  assembly active shows it and the step draws its trail; leaving it puts
  everything back, with no undo step and the document not modified.

Score before E2: 13 of 22 headless. The selection case failed on the open
transaction, and the 8 pose cases had no API. After: 22 of 22 headless,
23 of 23 in the GUI (`GT_ASSEMBLY_TESTS=AssemblyTests.TestExplodedView
scripts/gui-test.sh tests/gui/assembly-workbench-suite.py`). With E3:
26 of 26 headless (2 GUI-only skipped) and 28 of 28 in the GUI; with the
arc trails, 28 of 28 headless and 30 of 30 in the GUI. With E4: 32 of 32
headless (4 GUI-only skipped), 36 of 36 in the GUI. Before E4: the three
shape cases had no API; with the view provider's answer off, the drawn
case failed (the trail a plain stroke), and with its first colour the vg
page showed no trail at all. With the open items: 37 of 37 headless (4
GUI-only skipped) and 41 of 41 in the GUI; before, the three edit and undo
cases failed.

## 7. The later steps (E5 .. E7)

Left out of the first pass, as agreed, and taken up after it
("continue on later steps", 2026-11-14), in this order:
- E5, a drawing view per step (sec 8);
- E6, balloons carrying the BOM's numbers (sec 9);
- E7, auto explode: steps made from the joints (sec 10).

Tests and scores of all three: sec 11.

## 8. A drawing per step (E5)

**`Assembly::ExplodedViewAt` is the view at a step.** It holds the view
(`View`) and a progress (`Progress`, n the end of step n, below 0 the
end). Its shape is the view's there, `getExplodedShape(Progress)`, handed
out by `getSubObject` as the view's own is (sec 5). It writes nothing and
moves nothing: a TechDraw view whose Source is it projects the explode at
that step, and projects it again when a step changes. Its view provider,
`AssemblyGui::ViewProviderExplodedViewAt`, answers the drawing's
questions about edges as the exploded view's does, through `View`: its
trails are dashed too.

Found on the way, each a drawing left at the step as it was:
- **Part's shape cache kept its shape.** Part's cache of an object's shape
  is cleared when its `Shape`, `Group` or a touched property changes; this
  object has none of those, so a new `Progress` drew the old one. It
  clears its entry when it changes and when it recomputes.
- **The recompute skipped it, and its drawing.** With
  `OptimizeRecompute` (the default) an object recomputed only because what
  it depends on was is skipped when no input of its own changed. Both the
  `ExplodedViewAt` and the TechDraw view of it are such objects: an edit of
  a step recomputed the exploded view and stopped there. It is never
  skipped now (`skipRecompute()` false), and it marks what depends on it
  for recompute when it runs. With `OptimizeRecompute` off the drawing
  followed, which is how the cause was found.
- **`View` is a global link.** The exploded view is inside its assembly
  and this object is not, so a plain link warned on every recompute that
  it went out of its scope.

**Drawing of each step**, on the exploded view's context menu
(`CommandCreateView.makeStepDrawings`), in one undo step:
- a page on TechDraw's default template (the user's, else A4 landscape),
  unless a page is given;
- per step, an `ExplodedViewAt` at the step's end, kept in a group
  `ExplodedStepViews` beside the page, and a `DrawViewPart` of it;
- the views in a grid above the title block, at one scale, so a part
  keeps its size from view to view:
  - the scale fits the largest outline any step projects (a turn half
    open can stand taller than the end), with room round each view for
    its balloons (sec 9) and under it for its caption;
  - the grid is the one of 1 .. n columns that gives the largest scale;
    three steps on A3 are a row, not a square with a cell empty;
  - the scale is the fit to two figures, rounded down. Rounded to a
    standard scale (1, 2 or 5 times a power of ten) the manual's sample
    (three steps on A3) was drawn at 0.2; its fit is 0.49;
- under each view a caption, "k. <the step's label>";
- isometric by default; a direction can be given.

Step k shows the parts where the first k steps put them, and the trails of
those k steps, so the page reads as the sequence: what moved last is the
newest trail.

## 9. Balloons from the BOM (E6)

**Number parts in drawings**, on the context menu once a drawing of the
view exists (`CommandCreateView.addBomBalloons(drawing)`), puts a
balloon on each part a drawing shows, its number in the bill of
materials:
- the BOM is the view's `Bom`, else the assembly's, made if it has none,
  and kept in `Bom` after;
- one balloon a part, circular, its leader to the middle of the longest
  piece of it the drawing shows;
- the balloons on a ring round the view: the box of its visible edges
  grown by a balloon and a gap. Each stands where the line from the box's
  middle through its piece meets the ring, and neighbours are pushed apart
  along the ring until none meet. The first version stood each 12 mm out
  from its piece, and on a small view of eight parts four pairs met;
- run again, after a step, a part or the BOM changed, the balloons are
  laid on the ring again, the view's new one: a balloon whose number you
  typed keeps it, and a part gone takes its balloon. One you dragged stays
  where you put it against its part, and the others keep clear of it. A
  label records where the import stood it (`DrawEdgeLabel.ImportOffset`,
  against its edge); one standing elsewhere was dragged. A label from
  before the record (2026-11-15) is taken as dragged;
- no two leaders cross: after the spread, two balloons whose leaders cross
  change places, until none do. Each swap shortens the leaders in all, so
  it ends, and the places stay as spread. Parts in a row through the
  view's middle share a ray from it, and the spread crossed their leaders.
  A part's balloon is made with a straight leader (`KinkLength` 0), as the
  ring lays them: TechDraw's default shoulder could cross a neighbour's,
  which the manual's page showed.

How a drawing learns a part's number, with no TechDraw code that knows the
Assembly:
- **The edge's name says which object it is of.** A part's edges in the
  exploded shape carry their object's tag in their mapped names, and the
  projected edge keeps the name (sec 5). `ExplodedView::partNumberOf`
  reads the tag (`MappedName::findTagInElementName`, not recursive: the
  outermost, the part the assembly holds), finds the object by ID, and
  asks the BOM for its row.
- **The BOM answers for an object.** `BomObject::getIndexOf(obj)` gives a
  row's number for the object or anything linking to it, so a part
  brought in as a link is found; Python has `getRows()`, each row's
  number and object, and `getIndexOf`.
- **The view provider answers for the edge.** `getEdgeInfo` gives a part's
  edge its number as a label, once for the number, in a circular bubble,
  and leaves its line style alone. `Gui::ViewProvider::EdgeInfo` has three
  new fields for that: `styled` (false: draw the edge as the drawing
  would, only label it), `oncePerLabel` (one label for the text, not one
  for each edge) and `bubble` (the label's BubbleShape). The Python
  `getEdgeInfo` dict has them as `Styled`, `OncePerLabel` and `Bubble`.
- **TechDraw's Import Edge Labels does the rest**
  (`TechDraw_ImportEdgeLabels`, sheet metal's bend labels,
  `docs/SheetMetal.md`). For a once-per-label answer it keeps the longest
  visible piece of any edge with that text, and when the piece labelled
  before is gone it finds the label by its text and moves it to the new
  piece. `QGIViewPart` and `PageFeed` take a source's line style only when
  `styled` is set.

Found on the way: **a drawing was not drawn again when a part no step
moves moved**, by hand or by the solve. The view depends on its steps,
not on the parts it shows (they are its assembly's, which holds it), so
nothing told what draws it. When an assembly is recomputed now, its
views' shapes are made again and what draws them -- a TechDraw view, a
view at a step -- is recomputed: in the same pass if it comes later,
else in the recompute's second. Nothing inside the assembly is, as that
would touch the assembly again. Before, a part moved 100 mm up left its
drawing 30 mm tall (`TestExplodedViewRedrawn`, 2 of 2 failing).

Found on the way, too: **the BOM's mirrored check read memory it did not
own.** `BomObject::isObjMirrored` (upstream's) cast any object to a link
element with `static_cast`, never null, and read its `Scale`: for a
`Part::Box`, whatever lay there. A link is counted in its part's row only
when both are mirrored alike, so a full run of the explode tests once
numbered a link apart from its part. The casts are checked now; a
mirrored link (`Scale` -1) still gets a row of its own, which
`testBomRows` checks.

## 10. Auto explode (E7)

**Explode Automatically**, a button of the exploded view's panel
(`CommandCreateView.makeAutoExplode(assembly, view=None, factor=1.5)`),
adds steps made from the joints after the view's own, or makes a view
with them:
- the parts are the assembly's, an App::Part one part; the element
  bindings the joints hold are not parts;
- from the grounded parts -- the largest joined part when none is
  grounded -- the joints are walked outwards, each part reached once, by
  the first joint that reaches it;
- a part moves along its joint's axis -- the Z of the joint's frame on
  the side of the part it is joined to, in the assembly's frame -- the way
  out of that part: from that part's middle toward where the joint sits on
  it. The first version took the way from one middle to the other, and
  sent a bolt through a lid into the lid: the bolt's middle is below the
  lid's. When the joint sits on the middle (a pin in a hole), the middles
  decide;
- it moves by `factor` times its own size that way;
- the parts furthest out move first; a part takes with it the parts
  outside it, moved already, so an inner part's step does not run into
  them;
- parts at one depth moving the same way move in one step, labelled
  "Move" and their labels;
- a part no joint reaches is loose -- nothing holds it, it holds
  nothing -- and moves first, away from the middle of the box of the
  parts the joints reach, by `factor` times its own size that way; a
  loose part at that middle goes up (the assembly's Z). Loose parts going
  the same way share a step. With no joint at all, the largest part stays
  and every other one is loose (2026-11-15; it made no step before). A
  radial step was the other way: it moves a part by its distance from the
  middle, so one near it hardly moves; a step a way keeps the rule the
  joined parts go by. Found on the way: a part not recomputed yet had an
  endless box, and a move to infinity; a box not valid is taken as none.

The panel works it out from the design: it puts the parts back before,
and shows the view's steps after. The manual's housing, joined (the plate
grounded, the housing on it, the lid on the housing, the knob and four
bolts through the lid on it), gives three steps: the knob and the bolts
lift 24 mm (one depth, one way: one step, the bolts' 16 mm the largest),
then the lid 7.5 mm taking them, then the housing 45 mm taking all of
them.

## 11. Tests, E5 .. E7

In `TestExplodedView.py`:
- `TestExplodedViewAtSteps` (2): the shape at a progress, at the end, and
  after a step's edit, with no error; a page of each step, its captions,
  one scale, the trails of each, inside the page, and a step's edit
  projected again (GUI only).
- `TestExplodedViewBalloons` (4): the BOM's rows and a link's row; a
  part's edge's number from the view provider, a Body's numbered as the
  Body though its edges carry its features' tags (GUI only); the balloons
  on a drawing, one a number, circular, a balloon and a gap apart, and the
  same ones again (GUI only).
- `TestAutoExplode` (7): outer parts first, each taking those outside it;
  one step for a depth and a way; the largest part stays with none
  grounded; a part no joint reaches stays; the factor, after the view's
  own steps; a bolt through a lid leaves it outwards; the assembly's
  frame.

Scores. The tests were written with the code, before it was built; the
first run after building, headless: 7 of the 13 new failed:
- the 5 auto explode cases: a part directly in the assembly had no
  subname (`getParentGroup()` is None there; the assembly is a Part);
- the shape at a progress: Part's cache;
- the BOM's rows: the BOM had no Quantity column, so a link was a row of
  its own filed as the object it links. `assemblyBom` now makes the BOM
  as the BOM command does, and `getIndexOf` finds a row by the object as
  its parent holds it first.

In the GUI, 2 more: a part's edge had no number, the tag read being the
exploded shape's own (`;:H:9`), not the part's. `partNumberOf` now walks
the tags from the last inwards. Then the step drawing's re-projection: 1
of 2 failed before the recompute fix (sec 8); and the bolt through a lid,
added when the manual's scene showed it: 1 of 7 auto explode cases failed
before (the bolt sent 24 mm down). After: 54 of 54 headless (8 GUI-only
skipped), 54 of 54 in the GUI. TechDraw, sheet metal and Assembly
ctests: 54 of 54.

After the merge of main (2026-11-15), 1 more: the transaction log's row
of an edit then posed in one transaction, failing before (it held the
pose). 55 of 55 headless (8 GUI-only skipped), 55 of 55 in the GUI;
TestAnimation 59 OK.

After the balloon follow-up (2026-11-15): `TestExplodedViewRedrawn` 2
new, both failing before; balloons 3 new: laid again after a move and a
dragged one kept failed before (a balloon on the drawing), and leaders
crossing failed with the swap off. 60 of 60 in the GUI, 60 headless (13
GUI-only skipped); sheet metal commands 111, its edge-label re-import
included.

## 12. Open

- A dragged balloon is kept where it was put against its part, even if
  the view grew over it; delete it and run again to lay it.
- Leaders can cross the drawing's edges on their way to the ring; only
  crossings between leaders are taken out.
- A loose part goes straight away from the joined parts' middle, which
  can be a little up or down (the manual's bracket goes 2.2 mm down);
  nothing looks at what it would pass through.
