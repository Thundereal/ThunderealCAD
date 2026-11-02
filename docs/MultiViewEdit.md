# Multi-view editing: view state per view, edits per view

Status (2026-09-25): **idea recorded, not built.** Written down at the
user's request so it can grow before it is implemented. Nothing here has
code yet; where it names code, that is the code as it stands, which the
idea starts from.

Companion documents:

- `docs/TransactionLogJournal.md` sec 24.9-24.10 -- what the document log records
  of view providers today, and why the camera was kept out of it.
- `docs/ThinClient.md` 8.11 (the shared session) and 8.12 (what per client
  would cost) -- the multi-client side this idea answers much of.
- `docs/ViewSettings.md` -- per-view settings that override preferences,
  and what travels in a document.
- `docs/CoinRetirement.md` 5.9 and 5.12 -- the two per-view maps that
  already live on the view (`ObjectDisplayModes`, `OnTopObjects`).
- `docs/SplitViews.md` -- views with persistent names, which is what makes
  per-view state addressable across a save.
- `docs/TransactionLogJournal.md` sec 17 -- branches, merge and concurrent writers,
  which section 10 here builds on.

## 1. The idea (user, 2026-09-25)

A document's state splits in two, and the split is by *what the state
is about*, not by who changed it:

- **Document state** -- how the model is. Geometry, parameters, and the
  model's appearance: colours, materials, line widths. Shared by every
  view and every client, recorded by the transaction log, undone by the
  document's undo.
- **View state** -- how one view is looking at the model. The camera, the
  view's draw style and render settings, which objects it shows, which it
  draws on top, which display mode it forces. Owned by one view; every
  view has its own.

Two mechanisms follow:

1. **View state lives on the view, as view properties.** Each 3D view
   (`View3DInventor`, already a property container) carries its own values
   -- the camera included, as a plain string property.
2. **Each view has its own change stack, session only.** Changes to a
   view's properties are undone and redone per view, independently of the
   document's undo, and the stack is not saved and not logged. It dies
   with the view.

And a third, larger step that the first two make possible:

3. **The edit session moves from the document to the view.** Upstream
   FreeCAD has one edit per document (one object in edit, one task
   dialog); this fork extends it to one per view, so two views -- two
   windows, or two clients -- can edit different things at once.

Why it matters beyond tidiness: **each client of a served document has
its own view** (a mirror, `docs/ThinClient.md` 8.3). If view state is per
view, per-client camera, visibility and display modes come for free, and
several of the per-client items of `docs/ThinClient.md` 8.12 stop being
problems. The whole question then becomes *which properties are view
state* (section 3).

## 2. Where things stand

**Already per view** (on `View3DInventor`, saved in the view's `<View3D>`
entry of `GuiDocument.xml`): `DrawStyle`, `ShadingType`,
`ExternalRenderType`, `ShowNaviCube`, `ThumbnailView`, the light and render
overrides of `docs/ViewSettings.md`, and two per-object maps --
`ObjectDisplayModes` (a display mode forced on an object in this view,
`docs/CoinRetirement.md` 5.9) and `OnTopObjects` (5.12). The second of
those moved *off* the App document onto the view on 2026-08-27, which is
this idea applied once already.

**The presentation pattern exists.** `ViewProviderDocumentObject::
DisplayModeInView` is a transient property row on the view provider that
reads and writes the *active view's* `ObjectDisplayModes` entry for its
object. The storage is the view's; the property editor edits it where the
user expects to find it. Per-view visibility would be the same shape.

**The camera is ad hoc XML.** `Gui::Document::SaveDocFile` writes a
`<Camera>` element (the first view's settings, with `extra`, `id`,
`binding`, `view3d`, `viewareas` attributes), a `<CameraExtra>` per further
view, then a `<View3D>` per view (its properties) and a `<ViewArea>` per
split layout. `RestoreDocFile` reads them back into `_savedViews`,
`_view3DContents`, `_viewAreaLayouts` and rebuilds the views. The camera
is not a property and is in no undo.

**Visibility is two properties, both document state.**
`App::DocumentObject::Visibility` ("visibility in App name space") and
`ViewProviderDocumentObject::Visibility`, kept in step. Since
`docs/TransactionLogJournal.md` 24.10 both are logged and undone as document
data. `TempoVis` (the edit mode's temporary hides, a saved Python property
on e.g. `ViewProviderSketch`) writes the same visibility.

**The edit session is per document.** `Gui::Document::_editViewProvider`
(one object in edit), `_editingViewer`, the one editing root, and
`Gui::Control()` with its one active task dialog. The shared session of
`docs/ThinClient.md` 8.11 made that the definition: every view of the
document *joins* the one session (`View3DInventorViewer::joinEditing`),
so a sketch entered in one window is drawn in from any of them.

## 3. Which properties are view state

The test: **if a second person opens the same document next to you, should
your change change what they see?** Yes is document state; no is view
state. Applied:

| Property | Where now | Class | Note |
|---|---|---|---|
| camera (position, orientation, height/focal) | `<Camera>` XML | view | becomes a string property of the view |
| `DrawStyle`, `ShadingType`, render and light overrides | view | view | as today |
| `ObjectDisplayModes`, `OnTopObjects` | view | view | as today |
| object visibility (view provider `Visibility`) | view provider | **view** | the user's example; per view, seeded as below |
| `App::DocumentObject::Visibility` | App object | document | the seed for new views (section 4.3) |
| `ShapeAppearance`, `ShapeColor`, `LineColor`, `PointColor`, colour arrays, `Transparency`, `LineWidth`, `PointSize` | view provider | **document** | the user's example: colours are the model's look |
| `DisplayMode` (the object's own) | view provider | document | the per-view override is `ObjectDisplayModes` |
| `TempoVis` | view provider | view | the hides of an edit, which becomes per view (section 6) |
| `Selectable`, `ShowInTree`, `OnTopWhenSelected`, `SelectionStyle` | view provider | open | interaction preferences; see section 8 |
| `BoundingBox`, `Deviation`, `AngularDeflection`, `Lighting` | view provider | open | display quality; plausibly document (how the model is shown by default) with a per-view override later |

The camera and the view's settings are uncontroversial. Visibility is the
one that matters most and the one that changes behaviour, which is why
section 4 is mostly about it.

## 4. The view property mechanism

### 4.1 Storage on the view

View state is a property of the view. A per-object value is an entry of a
map on the view keyed the way `ObjectDisplayModes` and `OnTopObjects`
already are: an internal name, or `Doc#Name` for an object of another
document shown through a link, with a subname path where the value is per
occurrence. Per-object visibility becomes such a map (a hidden-object set,
or an explicit visible/hidden override per object -- to be decided, 8).

### 4.2 Presentation on the view provider

The property editor shows view state where users look for it -- on the
object -- through the `DisplayModeInView` pattern: a transient row on the
view provider that reads and writes the active view's entry. The view
provider's own `Visibility` row becomes that presentation. The many call
sites that read `ViewProvider::isShow()` or `Visibility.getValue()` need a
view to ask: the active one by default, the rendering view where there is
one (`ViewerContext::current()` is the existing seam).

### 4.3 Seeding and the primary view

A view needs values before anyone set any. **The App-space visibility is
the seed**: a new view shows what `App::DocumentObject::Visibility` says,
and copies nothing else from other views, except where a view is split
off another (`docs/SplitViews.md`), where it starts as a copy of its
parent.

**Only the host's primary view writes back.** Hiding an object in the
desktop's primary 3D view also sets the App visibility -- the document
still remembers what its author showed, the file still opens looking the
same, and scripts reading `obj.Visibility` keep working. Hiding it in a
second window, or in a client's view, changes that view only. Which view
is primary is a property of the Gui document (the first desktop view by
default); a served document's clients are never primary.

### 4.4 Saving

A desktop view's properties are saved in its `<View3D>` entry, as today,
and restored onto the view with the same persistent name. A client's
mirror view is not saved: its state is session state. The camera moves
into the view as a string property (`Camera`), saved with the rest of the
view. For readers of older formats the old `<Camera>`/`<CameraExtra>` XML
is still written when the document is saved at schema 4 and not at
schema 5 (user ruling, 2026-09-24); the reader takes both.

## 5. The per-view change stack

Each view keeps its own undo and redo stack of view-property changes:

- **Session only.** Not saved, not in the transaction log, gone when the
  view closes. The document's undo never touches view state, and a view's
  undo never touches the document.
- **Coalesced.** A camera orbit is one entry when the interaction settles,
  not one per frame; a run of changes to one property in one gesture is
  one entry.
- **Its own commands.** An "undo view" / "redo view" pair, the precedent
  being SolidWorks' Ctrl+Shift+Z "Previous View" and Rhino's `UndoView`
  (`docs/TransactionLogJournal.md` 24.10 has the survey). Which view it acts on:
  the active one.
- **Per client for free.** A client's view has its own stack; one client
  undoing a camera move or a hide does nothing to anyone else.

This is the mechanism the user ruled the camera should get instead of
the document log ("bundled with other property ops it will cause
trouble"), and the other view settings with it.

## 6. The edit session per view

Upstream FreeCAD edits one object per document at a time; this fork's
shared session (`docs/ThinClient.md` 8.11) keeps that and lets every view
join it. The extension is to allow **one edit session per view**: a view
may join the document's session, as now, or own one of its own, editing
another object while a different view (a window, a client) edits
something else.

What that touches, from `docs/ThinClient.md` 8.12:

- **D. The edit session.** `Gui::Document::_editViewProvider`,
  `getInEdit()`, `_editingTransform`, `signalInEdit`/`signalResetEdit`,
  `ViewProvider::setEditViewer`, the editing root: a view-to-session map,
  `getInEdit()` answering for the current view, an editing root per
  session. `ViewerContext::editViewProvider` is the seam that exists.
- **An object lock.** Two sessions must not edit one object: entering
  edit takes a per-object lock the other views see.
- **E. Chrome.** `Gui::Control()` has one active dialog; per-view sessions
  need a task panel per session (per view, or per client).
- **TempoVis per view.** An edit's temporary hides are view state by
  section 3: the view in edit hides, the others do not. That removes a
  whole class of "why did my other window change" surprises and the
  hides-as-document-state compromise 8.11 accepted.
- **Transactions.** Each session's edit is still a document transaction
  (the log records it); two sessions' transactions interleave in one log,
  which is what the per-session undo of `docs/ThinClient.md` 8.12 G and
  the selective undo of `docs/TransactionLogJournal.md` 24.4 are for.

## 7. What it changes elsewhere, when built

- `docs/TransactionLogJournal.md` 24.10 logs the view provider's `Visibility` as
  document data. Once visibility is view state, that row leaves the log;
  the App visibility stays in it (it is the document's record of what the
  author showed). `TempoVis` leaves it too.
- `docs/ThinClient.md` 8.12: A (the view) grows to hold the view state; E's
  "per-view visibility overrides, which exist today only as the browser's
  local hide" becomes the general mechanism; D becomes section 6.
- `docs/ViewSettings.md`: the per-view settings join the per-view change
  stack.

## 8. Open questions

- Visibility storage: a set of hidden objects, or a full override map
  (visible, hidden, follow-the-seed)? The second lets a view say "follow
  the document" per object.
- Where `Selectable`, `ShowInTree`, `OnTopWhenSelected`, `SelectionStyle`
  belong. They are about interaction rather than the model's look, which
  argues for view state; but a document author sometimes sets them on
  purpose for every reader (an unselectable reference body).
- Display quality (`Deviation`, `AngularDeflection`, `Lighting`,
  `BoundingBox`): document defaults with a per-view override, like display
  mode, or view state outright?
- Which view is primary when the desktop has several windows, and whether
  the user can change it.
- Whether the per-view change stack also takes view-affecting commands
  that are not properties (a section plane, a clipping box).
- Link and assembly occurrences: visibility per occurrence (subname) is
  already how `OnTopObjects` keys; `LinkVisibility` and element visibility
  (`setElementVisible`) need a place in the same scheme.

## 9. Order of work (later)

1. The camera as a view property, the old XML kept at schema 4 only.
2. The per-view change stack, with the camera and the existing view
   properties in it, and the undo-view/redo-view commands.
3. Visibility as view state: the map on the view, the presentation row,
   the seed and the primary-view write-back; the log's `Visibility` row
   changes with it.
4. The remaining classifications of section 8.
5. The edit session per view (section 6), on top of the shared session.
6. A client's view and session saved on its branch (section 10), on top of
   the concurrent-writer branches of `docs/TransactionLogJournal.md` 17.5.

## 10. A client's view and session as a branch of the log (user, 2026-09-25)

The pieces meet. `docs/TransactionLogJournal.md` 17.5 already rules that **every
writer works on its own branch** -- two clients in a shared session each
get one, created from the head they started at, merged into the shared
head after each operation (17.3), trimmed when the merge is clean. Section
6 here gives each view its own edit session. Put together: **a client's
view, with its editing session, is saved in the transaction log as that
client's branch.**

What the branch carries, beyond the ops it already holds:

- **The view's state.** The view properties of section 4 -- the camera,
  the view's settings, its per-object maps (display modes, on top,
  visibility) -- stored as one entity on the branch (the same bytes a
  desktop view writes into its `<View3D>` entry, so one serialiser, as
  sec 23.3 has it for everything else), rewritten at the branch's commits
  and at the view's settle points. The per-view change stack of section 5
  stays session only; what the branch keeps is the view as it stands, not
  its undo.
- **The edit session.** Which object is in edit and in what mode, and its
  view-state side (the `TempoVis` hides of section 6). The ops the session
  commits are the branch's ordinary transactions.

What that buys:

- **A session is resumable.** A client that disconnects, crashes or comes
  back tomorrow reopens its branch and gets its view back -- camera,
  visibility, display modes -- and re-enters the edit it was in, over the
  ops it had made. A desktop user gets the same from a named workspace.
- **Sessions are browsable.** The history panel (`docs/TransactionLogJournal.md`
  22.2) can list each client's branch with its view: what that person was
  looking at, what they had in edit, what they changed -- attribution and
  presence from the same records.
- **Conflicts keep their context.** When a client's merge conflicts
  (17.5), its branch stays; with the view on it, the conflict picker can
  show the other side as that client saw it.
- **Workspaces, Onshape's word, fall out.** A branch with a view and a
  session is a workspace in the full sense: a line of history plus the
  place someone is standing in it.

Where it rubs against 17.5, to settle when built:

- **Auto-trim.** 17.5 trims a writer branch once it merges cleanly; a
  branch that carries a client's view and session must outlive its
  merges for as long as the session does (or longer, if named). The rule
  becomes: the ops are trimmed (they are on the shared head as merge
  transactions), the branch row with its view and session stays.
- **Which view is the desktop's.** The host's primary view (section 4.3)
  is on `main` and writes the App visibility; every other view, desktop
  windows included, could be a branch of its own or share `main`'s --
  open, with the cost of a branch per window against the gain of
  resumable windows.
- **Size.** A view entity is small, but it is rewritten often; it goes
  through the reverse-delta policy of `docs/TransactionLogJournal.md` 23.2 like
  any other entity, and a camera settle point is not a commit.

## 11. The cross-document undo, to review with the log and per-view edit (2026-10-29)

**The mechanism.** `Document::_openTransaction` (`src/App/Document.cpp`):
a transaction opened in a document that is not the active one opens a
twin in the active document, under the same id and named `-> <name>`.
It is by design (user, 2026-10-29): it is what lets the user edit an
externally linked document in place and undo it from the document they
are working in. An implicit transaction is not mirrored (77f7b6428b,
`docs/TransactionLogJournal.md` 24.13).

**The case that shows it rubbing.** `tests/gui/serve-log-acts-browser.py`
serves a branch of a document in a document of its own
(`Gui.serveBranch`) and checks that nothing done there moves the first
document. The host's two changes on the branch's document each leave a
`-> ` twin in the first document's undo list, because the first is the
active document; the check "nothing the page did moved the document the
branch was served from" fails on the first document's `UndoCount` (4 at
`listed`, 6 at `restored`; its box unchanged). The journal last records
the test whole on 2026-10-15 (44 checks); when the active document
started being the first one in it is not bisected. Without a browser,
run through `scripts/gui-test.sh <script> <outdir>`:

    import os, tempfile
    import FreeCAD, FreeCADGui
    from PySide import QtCore
    from wsclient import free_port          # tests/gui on sys.path

    def run():
        p = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document")
        p.SetInt("TransactionLog", 2)
        doc = FreeCAD.newDocument("BranchUndo", hidden=True)
        doc.UndoMode = 1
        doc.openTransaction("base")
        box = doc.addObject("Part::Box", "Box")
        doc.recompute()
        doc.commitTransaction()
        doc.saveAs(os.path.join(tempfile.mkdtemp(), "log.FCStd"))
        for name, value in (("taller", 16), ("wider", 14)):
            doc.openTransaction(name)
            box.Height = value
            doc.recompute()
            doc.commitTransaction()
        FreeCADGui.serveDocument(doc, free_port())
        served = FreeCADGui.serveBranch(doc, "try")
        served.openTransaction("on try")
        served.getObject("Box").Height = 40
        served.recompute()
        served.commitTransaction()
        print(doc.UndoNames)    # ['-> on try', 'wider', 'taller', 'base']

    QtCore.QTimer.singleShot(1500, run)

**What to review, in view of the log and of edits per view.**

- The active document is a desktop notion. A served branch's document,
  a client's session, a view of its own (sec 6) each edit with no claim
  on the desktop's active document, and the mirror puts their steps in
  its undo list all the same -- an undo there then undoes a step the
  desktop user never took.
- With the log, a document's undo is its branch's rows. A `-> ` twin is
  a step in one document's list that is a row of another document's log
  (another branch, possibly another file); what an undo of it means for
  the first document's log -- a row, a cursor move, nothing -- is not
  said anywhere.
- With edits per view (sec 6), the in-place edit of a linked document is
  a view's edit, and the undo that should take it back is that view's
  (or that edit session's), not the active document's.
- So what the mirror is FOR -- one undo for an edit that spans documents
  -- may be better carried by the edit session (or the view) that spans
  them than by whichever document is active.

## 12. Undo steps by burst of work (user, 2026-10-29)

Answers sec 11. The user's idea: a transaction records changes by
itself, and an undo step is only a grouping the user (a person or code)
sees. So group by the work one event-loop pass does: the name comes from
`openTransaction()`, which may now be stacked, and every change in the
group -- whatever property, whatever document -- goes into its document
under the SAME id, so they undo together. What makes it work: garbage
collect the steps code opens and never closes (it relied on the next
open to close them), and decide what a long-running script's group is.

### 12.1 What a step already bounds

A step holds one old copy per property, taken on the first write
(`TransactionObject::setProperty`, `src/App/Transactions.cpp`): a
property set 10,000 times in a step costs one copy. An object made and
removed in the same step drops out; one changed then removed is one
removal. With the log on, `TransactionLog::onCommit` reads the same
entries once, at the commit: one op per touched property (the step's
copy before, the live value after), and nothing for an empty step. So a
step of any length costs at most one copy of what it touched; with the
log on the log grows with the number of steps, never with their length.
Coarser groups mean fewer rows, not larger ones.

### 12.2 The rules

1. **The group.** The work from one desktop input (a press, release,
   key, wheel, double click, context menu) up to the next input or to
   idle, whichever comes first. A 0-timer the input's work queues runs
   before idle and so is in its group: an input and the recompute it
   queued are one step. Without the Gui, an invocation (a script, a
   macro) is one group. A line typed into the Python console is a
   script's, not an input: it starts no group, and neither does a timer
   or a socket (12.6).
2. **One id.** Every document changed in a group gets a transaction
   under the group's id -- the step opened in it, explicit or implicit.
   A document with no step of its own joins the step last opened while
   it recomputes from a document that has that step open: an edit's
   bound step (sec 6) takes in the recompute it causes in a document
   that is not bound. A change made to that document directly is not the
   step's (`tests/gui/edit-two-documents.py` changes B in B's window
   while A's edit is open).
3. **The name.** From `openTransaction()`. No stacking (ruled, 12.6):
   outside a command an open commits the one before, as upstream's
   contract says; inside a command a nested open is dropped and the
   command's step and name stay (the `AutoTransaction` guard), which is
   joining, not stacking. Open/commit pairs one after another in a group
   stay steps of their own: a script keeps the steps it makes.
4. **Where a step is listed.** In every document it changed; and, as an
   empty pointer (`-> <name>`), in the document of the view the input
   that opened it went to, when that document did not change -- taken
   when the step is opened, from the receiver: its MDIView, else the
   view the dialog it is part of belongs to, else the active view. No
   input -- a timer, a socket, a script, FreeCADCmd -- no pointer. It
   replaces "the active document": an in-place edit of a linked
   document is still undone from the document worked in (the user's
   ruling, sec 11), and a script's or a served branch's step lands only
   where it changed.
5. **Holds.** A step lives past its group while held: by a running
   command (`AutoTransaction`), a `TransactionLocker`, a task dialog
   (a step opened while a dialog is up is the dialog's, and closes when
   it does -- today's `AutoTransaction::setEnable(false)`, which
   `Control::showDialog` and `Gui::Document::setEdit` call), an edit's
   own hold (typing, a spin box press), an editor that keeps its step
   over the inputs typed into it (set with `persist`: the property
   editor, the omni search box, the tree's label editor, which closes it
   when it goes), or a mouse button held down: a
   drag opens its step on the press (`ViewProviderDragger::onDragStart`)
   and closes it on the release, groups apart. Anything else still open
   at the end of a group is committed.
6. **Abort by id.** An abort ends only the step its opener opened, never
   another's -- after rule 5 committed one early, a late
   `abortCommand()` must find nothing to abort, not someone else's step.
7. **Log off.** A write with no step open is recorded too (an implicit
   step), not only with the log on.
8. **The log.** An empty pointer writes no row (`onCommit` skips an
   empty step -- already so); a pointer that took its document's own
   changes is that document's row.

### 12.3 Order of work

- **Stage 1** -- rules 2 and 4 with today's closing points (the Gui's
  0-timer for an implicit step, the end of an invocation, the command's
  return for an explicit one). Fixes sec 11's cases.
- **Stage 2** -- rules 1, 5, 6: the group's boundary, collection of
  what is left open, abort by id. Rule 3 needs no change (12.6).
- **Stage 3** -- rule 7, after measuring what recording costs a script
  that recorded nothing before (time; memory is bounded, 12.1). Done,
  12.8.

### 12.4 Measured before (2026-10-29, 06bbb5eabd)

Linked pair: B's Box changed, the recompute writes A's copy of it
(Part::FeaturePython over an XLink), A active, one Std_Undo with A
active:

| step opened by           | undo lists after                         | one undo   |
|--------------------------|------------------------------------------|------------|
| app-wide (a command)     | A `-> cmd step`, B `cmd step`             | both back  |
| `B.openTransaction()`    | A `-> script step`, B `script step`       | both back  |
| bound to B (an edit)     | A `-> edit step`, B `edit step`           | both back  |

In the third, `getActiveTransaction(A)` is None: the twin is A's only
way into the edit's step. Sec 11's case: the served branch's own step
lists in the first document (`['-> on try', 'wider', 'taller', 'base']`),
and a script's `other.openTransaction()` in the active one.

### 12.5 Stage 1 (2026-10-29, 4bde00ce23)

Rules 2 and 4 in, the twin gone. The pair of 12.4, after:

| step opened by           | undo lists after               | one undo   |
|--------------------------|--------------------------------|------------|
| app-wide (a command)     | A `cmd step`, B `cmd step`     | both back  |
| `B.openTransaction()`    | A `script step`, B `script step` | both back |
| bound to B (an edit)     | A `edit step`, B `edit step`   | both back  |

A gets a step of its own under the same id -- its recompute's -- not a
pointer. A timer's step that changes B only is listed in B only; a click
in A's view whose handler changes B only lists `-> click step` in A, and
undoes from there; a click in B's view, A active, lists it in B only
(`tests/gui/xdoc-undo-pair.py`). Sec 11's case: the served branch's step
stays in its own document (`doc ['wider', 'taller', 'base'] served
['on try']`), and a script's `other.openTransaction()` lands in `other`
only.

Rule 2 had first been "a document with no step joins the one opened
last, while it is open"; `tests/gui/edit-two-documents.py` failed on it
(a script's change to B, in B's window, joined A's open edit). Narrowed
to a recompute from a document the step is open in.

Gate: GUI 361/362 (LineWidths, known on main), Python 4062 OK, C++
1094/1095 (RenderGoldenCycles, known on main).

### 12.6 Open: rule 3 against the upstream contract

Stacked opens contradict what `openTransaction()` means today, outside a
command. Upstream's `Document.py` `testUndo`/`testUndoProperties`: open
T1, change, open T2 with no commit -- two steps, and aborting T2 takes
back T2 only. 272 `openTransaction(` calls in `src/Mod` Python (tests
aside) were written against that. Inside a command they already stack:
the `AutoTransaction` guard ignores a second open. Rule 1's collection
has a like edge: a console line is an input, so a step opened on one
line and committed on a later one would be collected in between; a
Python task panel's step (opened in a click, committed on OK) is held by
its dialog (rule 5) and is safe.

Recommended: drop stacking outside a command -- an open closes the one
before, as today -- and treat the Python console as a script, not as an
input boundary. Rule 1 then only commits what an input's own group
left open, which today the next open would have closed anyway.

Ruled by the user (2026-10-29): as recommended. Rule 3 reworded to
match (12.2).

### 12.7 Stage 2 (2026-10-29, 3f3d270d36, 723c7d5129)

Rules 1, 5 and 6 in, rule 3 as it was.

- **The group.** `GUIApplication::notify` counts each outermost input
  (the receiver's events of sec 12.2 rule 1) and tells the application
  (`setInputGroup`); a step minted then carries that group, one minted
  with `persist` or while `AutoTransaction` is disabled (a dialog, an
  edit) is marked held. A 0-timer queued at the input's end runs after
  the work it queued and calls `closeGroupStep()`, which commits the
  application's step only if it carries a group and nothing holds it:
  no command running (the guard), no `TransactionLocker`, no task dialog
  open, no document in edit, no mouse button down. A bound step (an
  edit's) is never touched. A console line sets group 0.
- **Editors.** The property editor, the omni search box and the tree's
  label editor set their steps with `persist`. The tree's never closed
  its step (the next step did); it now closes it when the editor goes.
- **Rule 6.** `Command::abortCommand()` does nothing when the step its
  `openCommand()` set was committed by its group.
- **Off switch.** `Document/UndoGroupByInput` (on); off, a step left
  open stays open until the next step is set.

`tests/gui/undo-group-by-input.py`: a click's loose step is committed
after its work and a later click's change to another document does not
join it; one undo takes back only the click's; held by `persist`, by a
task dialog, by a mouse button down (committed after the release); a
console line's step stays open; off, it stays open. Rule 6 is not
reachable from Python and has no test.

Typing into the console in that test crashed FreeCAD: the call tips
list re-sent a key release to the console through its own application
filter until the stack ran out (since 8f3ae4742e; a real keyboard
reaches the popup instead). Fixed in 401e3b1aab.

**Undo steps, rules on against off.** The whole GUI suite twice on the
same binary, `UndoGroupByInput` on then off, an observer counting per
test the steps opened (named, pointer, implicit), aborted and
committed, and the undo list of each document a test closes:

| per run (331 tests)         | off  | on   |
|-----------------------------|------|------|
| named steps opened          | 627  | 626  |
| pointer steps (`-> name`)   | 1    | 1    |
| implicit steps              | 1905 | 1907 |
| aborts                      | 64   | 64   |
| commits                     | 2480 | 2481 |
| undo steps listed at close  | 432  | 432  |

Two tests differ. `GuiUndoGroupByInput` is the new test, which fails
off by design. `GuiSyncViewDoesNotMesh` had one more implicit recompute
in one run; four repeats, two each way, give the same tally. So the
rules change no test's number of steps. Most tests run from a timer,
which starts no group; of the 141 that send input, the step set in an
input is a command's, closed at its return, or a script's, between
inputs. Rule 1 commits only what the next open would have closed, and
the suite has no such step. What changes is what a later input's
change joins, which `undo-group-by-input.py` shows. The Python suite
(FreeCADCmd) has no inputs and is the same by construction.

Gate: GUI 362/363 (LineWidths, known on main), Python 4062 OK, C++
1094/1095 (RenderGoldenCycles, known on main).

### 12.8 Stage 3 (2026-10-30, 15785faed1, 97630abae1, ea98250cd0)

A document with no log already had implicit steps before rule 7: a
temporary one, a version, a replay keep none while TransactionLog is 2,
and `_openImplicitTransaction` asked only the preference. They went on
the undo stack as they do with a log, and three things there assumed
one:

- **The limit counted them.** `_trimHotWindow` turns old steps cold with
  a log; without one it deletes them, and counted implicit steps as
  steps: 25 steps with an implicit one after each left 10 of the 20.
  Now the limit counts the user's, a trimmed one takes the implicit ones
  above it, and implicit ones under the first user step left go.
- **Runs piled up.** A run of implicit steps only ever undoes whole (sec
  33.4). `_foldImplicit` folds an implicit step into the implicit step
  before it (`Transaction::absorb`, the oldest copy of each property
  kept), and drops one with no user step under it, which nothing undoes.
  A step in which an object came or went is not folded (`absorb`
  refuses it).
- **Redo.** With a log, a redo after an implicit write goes through the
  log. Without one its copies are of a state the write has left, so a
  write by hand ends the redo steps. A recompute's does not: undo,
  recompute, redo is what upstream code does all the time
  (TestSolidBody, TestPatternKind, TestLinearPattern), and the redo's
  copies put back what the recompute wrote, which the next recompute
  writes again (97630abae1, after the first version broke those three
  with the log off).

The log is not made early to answer "is there a log" (`d->transactionLog`,
not `getTransactionLog()`): made at a step's open instead of its commit,
it keeps an object removed in that step for its row, its name taken till
the commit, and two CAM post tests that add and remove `testpath` in one
uncommitted step failed on it.

Rule 7 then (ea98250cd0): with TransactionLog 0, `_openImplicitTransaction`
opens the step too, and a removal with nothing open is one, where it was
a "Delete" step of its own when the object was in an earlier step and
nothing otherwise. `Document/UndoImplicitLogOff` (on) turns it off. A
temporary document's removal is left as it was: held for an undo, the
removed object would keep its name taken in a workspace that reuses it.

**Cost**, log off, one user step under it all, FreeCADCmd, three runs
each way:

| work                                          | rule 7 off       | rule 7 on          |
|-----------------------------------------------|------------------|--------------------|
| 2000 x write a box, recompute, commit         | 0.85-0.90 s, +1.8 MB | 0.87-0.89 s, +3.2 MB |
| 500 x make a box (one step), remove it (next) | 0.33 s, +0.6 MB  | 0.36-0.40 s, +63.5 MB |
| the one undo after                            | 0.000 s          | 0.065 s            |

The first is the fold: 2000 steps kept as one, the oldest copies of 50
boxes. The second is the price: each removed object is held so the undo
can bring it back, about 127 KB a box here, in 1000 steps, as neither a
making nor a removal folds. Open: fold a making and a later removal of
the same object in one run to nothing, as one step already does and the
log's squash does (`NetChange`, sec 16.7); that takes the second case
back to about nothing.

**Gates.** Defaults: Python 4064 OK, C++ 1094/1095 (RenderGoldenCycles,
known on main), GUI 362/363 (LineWidths, known on main) -- the C++ and
GUI runs before 97630abae1, which changes only a document with no log.
Python with TransactionLog 0, rule 7 on: the three BoundTransactionCases
that read log rows fail, as they do with it off; off, one appearance
test (`testAColourWrittenToABinderEndsItsMapping`) fails besides, and
passes with it on.

### 12.9 Each view's undo (entry 227, user 2026-10-30)

Entry 227 asked what a view's undo offers: a fillet's edit step, made in a
3D view, was offered on a TechDraw page. Every view listed and took the
one document stack. The ruling: "for 3D views it is 3. other views 1",
and yes to both points put with it.

- **A 3D view** (option 3): while an edit is open in it, its undo lists the
  edit's own steps (`Gui::Document::editStepsOf`, what Cancel takes back)
  and Std_Undo takes the newest of them -- in a view that joined the edit
  too, as every 3D view of the document does with PerViewEdit off. With
  none open, every step --
  but the steps of an edit another view has open: undoing those under its
  open dialog is that view's to do.
- **Any other view** (option 1) -- a page, a spreadsheet: the steps made in
  it, and those made in no view.
- **Where a step is made** is where its input went: a widget of a view is
  that view's; a task panel's, the view of its dialog; the tree, the
  property view, any other dock and the Python console are no view's, nor
  is a script's step with no input; a toolbar, a menu, a shortcut, the
  active view's. A step keeps its view through undo and redo, and two
  writes of one property fold (sec 37.4 P4 of the journal) only within
  one view.

How. `App::Transaction::View` is the view's number
(`MDIView::undoViewId`), taken as the step opens from the input's
(`Application::setInputView`, set with the input document by the Gui's
input scope; carried with the application's active transaction as its
listing document is, `getStepView`). `App::UndoViewScope` names what the
view asking lists (`Gui::Document::undoViewOf`); `Document::_myRun` and
the lists read it, passing over a step the view does not list with the
implicit steps that go with it, as they pass over another author's. The
Gui sets it around a view's `undoActions`/`redoActions` and the
Undo/Redo messages to the active view (Std_Undo, Std_Redo, their
drop-downs and their enabling).

A step taken from under others goes through the log (sec 30.10 of the
journal), refused where a later step changed what it did, as an author's
undo is. With the log off (`TransactionLog` 0, for A/B checks only) there
is nothing to take it through: every view lists every step, as before.
A new step still ends the user's redo steps of every view, as it ends
the user's whatever they were.

A view that closes. A TechDraw page or a spreadsheet has one view at a
time (its view provider keeps one and brings it to the front when asked
again; a split cell is given a 3D view instead, `ViewArea::cloneChildFor`),
so its number is the object's, not the window's
(`Gui::Document::undoViewIdOf`, by object id, for as long as the document
is open): opened again, it lists the steps made in it before. Several
browser clients may be served one page (`TechDrawGui::PageServe`) or one
sheet, but a client is no MDIView: its undo is the `undo` op, which runs
as the connection's actor and lists that user's steps (sec 30.10 of the
journal), so clients are told apart by who they are, not by a view. Any
other view that is not a 3D view hands its steps to every view as it
closes (`App::Document::releaseStepView`); a 3D view keeps them, as no 3D
view tells them apart, and a page does not list them.

An edit's Cancel with another view's step among the edit's goes through
the log, as a step of its own that takes the edit's rows back
(`Document::undoLogged`). That step is made in the view its input went to,
as any step is: the Cancel of the edit's own panel is that view's, not a
step every view lists. Refused for a step made since that depends on the
edit's, it asks as an undo refused for one does (sec 12.11).

Tests: `tests/gui/per-view-undo.py`; `tests/gui/td-model-interleave.py`, a
drawing and the model it draws each in edit in a view of its own, their
changes interleaved with recomputes, undo and redo from either view.

### 12.10 What an undo leaves to recompute (user, 2026-10-31)

Found by `td-model-interleave.py`: after an undo of a change to the model,
the drawing showed the model as it was before the undo, and nothing was
marked to recompute. The user asked for the touched state an undo leaves
to be checked first: a step made from a clean state should be undone to a
clean one, needing no recompute.

What the undo does (measured on a box drawn by a part view, its length
changed, recomputed, undone, redone; `FreeCADCmd`, log on and off). With
the log on, an undo leaves each object the touched state the rows say it
had before the step, and a redo the state before the undo (sec 27.63 of
`docs/TransactionLogJournal.md`): a step made clean is undone clean, one
made over a touched box leaves it touched. The box's length and shape come
back together, as the shape is in the step (the recompute's writes ride
with the step under them). With the log off every object an undo or redo
writes is touched, as upstream does, and the next recompute makes it all
again.

What went wrong is the drawing, not the state. A part view keeps its
projection in the view (`DrawViewPart::m_geometryObject`), not in a
property, and a dimension keeps its measured points the same way; no undo
puts them back. The rows say the view was clean before the step, and that
is what it was told, so it showed the 20 mm box with the length back at
15 and nothing to say so.

`DocumentObject::resultInProperties()`, true by default, false for
`DrawViewPart` (and what derives from it: section, detail, projection
group item) and `DrawViewDimension`. After an undo or redo applies its
writes, such an object is touched when the writes (the step that takes it
back again) hold the object or anything it depends on -- with the log, in
the set touched last as a cold undo's lost derived values are
(`StepTouched::leave`), so the undo's row records it; through the log
(`_revertStep`), with the values that revert leaves to its recompute. With
the log off the box is left touched, and the next recompute would make the
view again anyway; it is touched all the same, so the tree says the same
in both modes.

`Document/RecomputeOnUndo` (off by default): a run of undos or redos that
leaves objects to recompute -- ones touched when the step was made, ones
whose result an undo cannot put back -- is followed by the recompute, once
the run is all taken, not between its steps
(`App::Document::recomputeAfterUndo`, called by `Gui::Document::undoOrRedo`
and the Python `undo()`/`redo()`). The steps another document took in a
grouped run are left alone.

Test: `src/Mod/TechDraw/TDTest/DrawViewUndoTest.py`.

**Corrected at the source (user, 2026-10-31): "this is a design error and
we shall correct that."** The projection was in a property all along
(`ProjectedGeometry`, `docs/TechDrawStoredGeometry.md`), and the undo put
it back; the view read it only when a file was opened. It reads it after
an undo or a redo now, a dimension measures when read, and the 3D shapes a
section or detail keeps for the views made from it, and a hatch's trimmed
lines, are stored with the projection (`docs/TechDrawStoredGeometry.md`
sec 6 to 8, user 2026-11-01: an undo in memory constructs nothing). An undo
leaves a drawing right and clean; `resultInProperties()` stays as the
fallback, false for a part view only while the store is switched off or a
projection is running.

### 12.11 An edit's steps are one undo (user, 2026-10-31 and 2026-11-01)

The user's model: "group all editing undo steps including the recompute
before exit into one super undo step. Change ui to show expansion of small
steps in the super step for user to optionally fine undo. Clicking the undo
button is coarse." Ruled on the open questions: the drop-down is a menu
whose group row takes the whole edit and opens its steps on hover; every
edit ended with OK groups, Sketcher's too; a step of another view or user
between the edit's is passed over; the browser gets the grouping and its
list now; a dependency between steps is checked by reference as well as by
property, and a step of another user in the way is undone only by asking
that user.

**The group, in App (built).** `Transaction::Group` keys the steps of one
edit: the seq of its oldest row, or with no log its oldest step's id
negated. `Document::groupSteps(ids)` makes one (Python `groupSteps`);
implicit steps are not marked, they ride on the step they follow. The key
is written to the rows, `txn.grp`, store layout 4 (a migration adds the
column); the stacks rebuilt from the log keep it, an inverse step keeps
its step's, and a cold stub its own. Those are a crashed session's
recovered and a branch's arrived back on, a group on either stack one
undo or redo again (gtest `aGroupIsOneStepAgainOnStacksRebuiltFromTheLog`);
a plain open starts with no steps (TransactionLogJournal sec 26.4).

`Document::_myRun` gathers a group: on undo, from its newest step down
through the stack, taking each step of the group with the implicit ones
riding on it and passing over every other step -- another view's, another
user's, or one of no group -- which stays as it is; the steps under one
passed over are taken back from the log, out of order, and the run is all
or none (`_takeRun`). On redo the same from the other end. `runStopAt`
ends the run at a step of the group: `undo(id)` with a step inside the
group at the top takes the group down to it and no further (Python
`undoTo`, `redoTo`), what is left a group still on each stack. A run
skipped past a refusal (sec 33.11) skips its group whole.

`getAvailableUndoSteps()` / `getAvailableRedoSteps()` list what an undo of
the view lists with each step's id and group (Python `getUndoSteps`,
`getRedoSteps`).

Gtests `aGroupOfStepsIsUndoneAndRedoneWhole`,
`aGroupIsUndoneFromUnderAnotherStepAndInPart`; the migration tests check
layout 4.

**Grouped when the edit ends (Gui, built).** `Gui::Document` groups an
edit's steps as its session ends (`_resetEditSession`), once the event that
ended it is done -- the step OK opened is committed then. The group is the
steps the session recorded, the step that was open when the edit began
(the command that started it, a new object made in it: `firstStep`, which
Cancel does not take back), and every step above the stack's top as the
edit began (`startStep`) that no other running edit recorded and no group
holds -- a panel's OK step is opened outside any view's event and no
session records it (PartDesign's "OK, recompute"). Ended by Cancel, the
steps are taken back first and grouped on the redo stack: one redo brings
the edit back.

**The drop-down (built).** `Gui::Dialog::UndoRedoMenu`, behind both the
undo and the redo button: one row a step or a group, a group's row a menu
of its steps, named by the step the edit began with and how many it has.
Picking a row undoes down to it, a row an undo each; clicking a group's
row -- not only hovering it -- takes the edit (Qt triggers nothing on a
submenu's row: the menu handles the release and Enter); picking a step in
it undoes the rows above, then the edit down to that step
(`Gui::Document::undoTo`, `App::Document::undo(id)`). A view whose undo
is not its document's -- a text editor -- lists names, as before
(`MDIView::undoSteps` empty). GUI test `tests/gui/edit-group-undo.py`.

**What stands in an undo's way (App, built).** A step taken back from under
steps made since (out of order, from the log) is refused when one of them
depends on it, and the refusal now says which: `stepsInTheWay()` lists the
steps on the undo stack that made those rows, each with its author (Python
`getStepsInTheWay`). A step depends on the one undone when it
- changed since a property that one set (`checkSelective`, as before);
- linked to an object that one made, which the undo removes and would
  leave the link to nothing (`linksMadeSince`, before anything moves): a
  Pad made in another view on a sketch the edit made;
- set a reference the undo breaks: once the values are back, every link
  with a sub-element and every expression of the objects referring to what
  the undo wrote is resolved again, and one that resolved before and does
  not now, set by a later row, refuses the undo -- put back as it was --
  naming that row: an expression `A.Extra` on a property the undone step
  added.
A reference already broken before the undo, or one set before the undone
step, does not refuse it. `undoSteps(ids)` undoes the steps given with the
implicit ones riding on each, newest first, all or none, whoever's (Python
`undoSteps`): an undo with the steps in its way. Gtests
`aLinkMadeSinceToWhatAStepMadeStandsInItsWay`,
`aReferenceTheUndoWouldBreakStandsInItsWay`.

**The dialog (Gui, built).** An undo of one's own (the button, Ctrl+Z, the
list) refused for steps in its way says them -- each by name, and by its
author when another's -- and offers:
- **Undo with them** (the default): the undo and those steps together,
  as one run (`undoSteps`), and every further step of one's own found in
  the way on the way; not offered when one of them is another's, which is
  that user's to agree to (sec 12.11, asked as the next part does);
- **Keep theirs**: the undo with what they changed since left as it is
  (`undoKeepingTheirs`, the log's `UndoConflicts::Skip` for every step of
  the run): offered only when what stands in the way is properties set
  again, not a link or a reference made since;
- **Cancel**.
"Undo anyway", which put the step's values back over a later one's while
that step stayed in the history, is not offered: undo with them says the
same honestly. A TechDraw page's undo asked about nothing it could not
take (it called the document's undo without asking); it asks as a 3D
view's does. GUI test `tests/gui/undo-in-the-way.py`.

A panel's Cancel refused the same way -- its edit's steps taken back from
the log (sec 12.9), a step made since in the way -- asks with the same
dialog (user, 2026-11-01), in place of its own "Undo anyway / Skip /
Keep": undo with them (or ask their owner), keep theirs (the log's
`UndoConflicts::Skip` for the edit's rows, what "Skip" was), or "Keep the
edit", taking back nothing -- the edit has ended either way. For that,
`undoLogged` of several rows names the rows in its way, as `stepsInTheWay`
reads them, where its check before any is taken back said them only in
the refusal's text. Refused for anything else, Cancel says why. GUI test
`tests/gui/pd-edit-two-views.py`; gtest `severalRowsUndoneAreOneStep`.

**Asking the other user (built).** A step of another user's in the way is
theirs to agree to: the undo with it is asked for, and goes once they do
(`Gui::UndoConsent`). The request holds the steps the undo takes -- the
asker's run and those in its way -- what was asked for, the owners'
steps by name, whose agreement it waits for and who asked. Each served
owner is asked on every connection of theirs (`consent.ask`, pushed by the
serving tier's transport); the desktop user in a dialog that does not stop
the desktop (`undoConsentAsk`). An owner with no connection cannot be
asked, and the asker is told so at once. Every owner's yes makes the undo
(`undoSteps`, as the one who asked: the inverse rows are theirs); one no
ends it; the asker is told either way (`consent.result` to a served one, a
dialog that does not stop the desktop to the desktop user).

On the wire (`SceneControl.cpp`): `undo` and `redo` answer with each
listed step's id and group beside the names (`undoSteps`, `redoSteps`);
`to` takes the list's fine undo to a step; `keep` is keep theirs; `with`
the steps in the way, the asker's own taken at once, another's asked for
(`Asked` with the request's id, or `NotConnected`); a refusal for steps in
the way is `InTheWay` with them, their authors and `keepable`. `undo.list`
reads the stacks; `consent.answer {request, allow}` is an owner's answer,
taken only from that owner. On the desktop the dialog's "Ask <user>" asks
the same way. GUI test `tests/gui/serve-undo-consent.py` (synthetic socket
clients: a group listed and undone, bob asked and agreeing, bob saying no,
the desktop user asked in its dialog and agreeing, bob gone).

**The browser's list (built).** A third button in the viewer's corner
(`web/src/undolist.tsx`) opens what this user's undo and redo would take,
read with `undo.list`: one row a step or a group, a group's row opening to
its steps. A row taken undoes down to it (`undo {to}` of its last step: a
group's whole edit); a step of a group, the edit down to that step. An undo
refused for steps in its way raises a card in the list naming them and
whose they are, offering "Undo with them" -- "Ask <user>" when one is
another's -- "Keep theirs" when it may be kept, and Cancel; asked, it says
it waits. A question put to this user (`consent.ask`) is a card over the
view with Allow and Deny; what an asked user answered is said for a while
(`consent.result`). A refusal's whole answer reaches the caller now
(`control.ts` rejects with every field of it, its code and message on
top). GUI test `tests/gui/serve-undo-browser.py` (Chrome on two pages,
alice's and bob's, through `scripts/undo-drive.js`).
