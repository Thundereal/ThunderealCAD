# TechDraw recompute errors: what says why, and what does not (entry 178)

Asked by the reporter 2026-10-19 12:11, through the note-taker's queue:
"check my live session and console log find out what causes recompute error
of Dimension120, add modify techdraw code to output the error description in
execute(). audit TechDraw code for missing error description in execute()".

## 1. Dimension120 (done)

Read in the reporter's live session (read-only) and its console log: an
ANGLE dimension on SectionView004 between Edge5 and Edge9, both at 45 deg --
parallel. After the section changed, neither reference matched exactly
("no exact match for changed 2d reference: 0 / 1") and the geometry
comparison repointed them onto two parallel edges. The error read "Invalid
selection - Det = 0" (Generic::apparentInter), naming nothing.

Built: `DrawViewDimension::execute()` turns a failure of its measurement
into a recompute error that says the kind, the view, the elements and the
reason, and adds, when the references were matched again by geometry, that
they should be checked; the angle code says "the edges Edge5 and Edge9 are
parallel, so there is no angle between them" before apparentInter() can.
Also fixed: the "second 2d reference is a ..." message named the first
reference's type. Test: TDTest DrawViewDimensionTest
testAngleOnParallelEdgesSaysWhy.

## 2. The audit: one structural cause, then the messages

**The structural cause.** DrawViewPart, DrawViewSection, DrawViewDetail and
DrawComplexSection start their real work -- the hidden-line projection, the
face finding, the section cut, the detail cut -- on worker threads, and
`execute()` returns success before it is done (`DrawViewPart.cpp` 296-298:
`partExec()` then `return DrawView::execute()`). Whatever fails afterwards
cannot become the recompute error: it is a console line, and the view is
left empty or partial. Every view type rides this path, so most of what a
user meets as "the view is blank" or "the dimension broke" starts here.

Read and confirmed for two of them: `DrawViewPart::execute` logs "Source
shape is Null." and returns success (285-288); `DrawComplexSection` throws
"Profile is parallel to Section Normal" (332) and its own `catch (...)`
(392-395) replaces it with a console line.

**The rest, by class** (an Explore pass over `src/Mod/TechDraw/App`; file:line
as found, only the two above re-read by hand):

| Class | Silent (nothing reaches the error) | Weak (generic, no context) |
|---|---|---|
| DrawViewPart | source shape null (285); HLR failed in the worker (440-448); face finding failed (504-512, 727-730, 964-968); projection CS fallback (1537); hatch reference lost (643-650); fusion fallback (ShapeExtractor 202) | -- |
| DrawViewSection | base shape null (445); SectionOrigin outside the part (455, a Warning); the cut failed in the worker (555-568); a solid's cut failed (610); prism and input disjoint (672); prepareShape (733); naming failed (846); section face (1129); section CS fallback (1438); hatch file unreadable (1565) | "BaseView object not found" (440); "SectionNormal and XDirection are parallel" escapes a Qt slot (1453) |
| DrawComplexSection | "Profile is parallel to Section Normal" and "Can not make wire (2)" swallowed (332, 338 -> 392); worker failure (381); profile outside the shape (182) | "Can not make wire from cutting tool (1)" (168) |
| DrawViewDetail | no BaseView / not a part view / base shape null, all success (148-165); tool or detail failed, left empty (278-291, 338-359, 391-400) | -- |
| DrawViewDimension | view with no geometry keeps stale points (403, 545); an unknown reference type "succeeds" (481-494) | "invalid 2d references" names no reference (446); "Geometry for dimension reference is null." also for the wrong type; "(1)..(4)" codes |
| DrawViewDimExtent, LandmarkDimension | no source / references: success (72-80; 103) | raw exception text (89) |
| DrawHatch, DrawGeomHatch | a lost face reference never reported; pattern file unreadable (266) | -- |
| DrawBrokenView, DrawViewMulti, DrawViewSpreadsheet, DrawViewArch, DrawViewSymbol, DrawParametricTemplate | source null, a failed break ignored, bad cell range, not an ArchSection, invalid SVG, template unreadable | raw OCCT / Python text with no view named |

Nothing to fail: DrawViewBalloon (it does not check SourceView), DrawLeaderLine,
DrawRichAnno, DrawWeldSymbol, DrawTile(Weld), DrawViewImage,
DrawViewAnnotation, DrawViewClip, DrawViewCollection, DrawPage.

## 3. Proposal

1. **A late error for the threaded views.** `App::Document::setErrorDescription`
   exists. A small `DrawView::reportLateError(text)` -- called on the GUI
   thread from the worker's finished slot when the worker failed -- sets the
   object's error status and description and tells the views, so the tree
   shows the error icon and the tool tip says why; the next recompute
   clears it. The workers already catch their exceptions; they keep the
   text instead of only logging it. One mechanism, used by DrawViewPart
   (projection, faces), DrawViewSection (cut), DrawViewDetail (detail) and
   DrawComplexSection (aligned pieces).
2. **Synchronous failures return a description** instead of logging and
   returning success: a null source, no base view, the section origin
   outside the part, a lost hatch face, the complex section's swallowed
   messages -- each `return new DocumentObjectExecReturn("<kind> <name>:
   <reason>")`.
3. **Dimension messages**: name the reference and the view in "invalid 2d
   references"; replace the "(1)..(4)" codes and "Geometry ... is null"
   with what was expected and what was found; the extent and landmark
   dimensions through the same `described()` wrapper.
4. Tests: one per class touched, a case that fails on purpose and reads
   the message (as testAngleOnParallelEdgesSaysWhy does).

**Agreed 2026-10-19 13:12 ("agree all")**: a-c as proposed below. **3.1 BUILT
(`9746b8aba9`)**: `DrawView::reportLateError` / `reportLateWarning`, used by
the part view (projection: error; faces: warning), the section and detail
cuts and the complex section's aligned pieces (errors). An error reported
inside the object's own recompute is held and returned by it (a success
would reset it). Found on the way: the projection worker destroyed its own
watcher (`abortMakeGeometry()` on the worker thread), so its finished()
never came. Next: 3.2, then 3.3.

### For the reporter

a. How far: all of 3.1-3.3, or 3.1 and the dimensions first? Proposed:
   3.1 first (it is what makes a blank view say why), then 3.2 and 3.3.
b. A failure that leaves a view usable but partial -- one solid's cut
   failed, a break ignored, a fusion fallen back -- an error, or a warning
   in the tool tip while the object stays valid? Proposed: a warning; only
   a view with nothing to show is an error.
c. The messages' wording is translatable text (QT_TRANSLATE_NOOP) -- yes?
   Proposed: yes, as the rest of TechDraw's user-facing strings.
