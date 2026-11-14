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
    segments, so an arc is several, and its view provider draws each.
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

## 5. Drawings (E4)

**An exploded view has a shape.** `ExplodedView::getExplodedShape(progress)`
is the assembly as the steps put it, in the document's frame, writing
nothing:
- what the assembly shows, its parts moved where the steps put them; a
  part inside an `App::Part` is moved inside it, the rest of that Part
  drawn where it is;
- the trails as edges named `Trail<n>`, and `Trail<n>Part<k>` for the
  pieces of a turn's arc (sec 2).

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
- `TestExplodedViewShown` (9, E2):
  - the tree's explode and restore are not an edit;
  - pose and re-pose from rest; the trails and the steps' visibility;
  - explodedAt while posed;
  - a part in a Part that a step also moved;
  - a save while posed writes the design; a recompute while posed solves
    the design (a revolute joint), the pose back after;
  - an edit while posed is the design;
  - deleting the posed view.
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
  - a turn's trail as Trail1 and its pieces;
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
page showed no trail at all.

## 7. Open

- Editing a step (its transform, its parts) while the view is posed does
  not re-pose. The tree's explode ends when the selection moves, and the
  panel unposes first, so neither user path meets it.
- An undo that records a part's placement while posed records the pose
  as the value before. Animation answers this with `restValueOf()`
  (Animation.md sec 5.2); the exploded view does not take part yet.
- A part's rest bounding box is that of its shape as the document has
  it. A part that is an `App::Part` holding parts a step also moved is
  boxed as posed.
