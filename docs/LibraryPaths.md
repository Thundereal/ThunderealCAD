# Library paths: links into named folders

Status (2026-10-18): **design ruled whole; steps 1 to 3 built** (App, Gui, the log;
sections 7 to 9). Step 4, the export, waits for TransactionLog sec 35 L7.
Asked for by the user the same day. Q1 to Q8 are ruled (section 5).

## 1. The ask (user, 2026-10-18)

A document-wise management of the folders external links point into:

- a **system setting**: user-defined names, each mapped to an absolute
  folder -- a *library path*;
- a link to a file under one of them saves the common part of the path
  as a token, `%name%`, and not as a path;
- the document **copies the library paths it uses** into a property of
  its own, a map;
- resolving a link's path tries **the document's copy first**, then the
  system's;
- care is needed where the document's map and the system's disagree.

## 2. What exists (survey)

- **`PropertyXLink`** keeps `filePath` and a `PathResolveMode`
  (`PropertyLinks.cpp`, `getPathResolveModeDocs`): `Dynamic` (the default:
  a relative path, resolved against the owner's file, then searched for
  through any symlinks on the owner's path, `resolvePath`), `Relative`,
  `RelativeCanonical`, `Absolute`, `Canonical`. `DocInfo::getDocPath`
  turns a target into the saved form and the full path; `getFilePath(mode)`
  gives a link's path in any mode, and is what Python and the property
  editor show.
- **A relative form needs the owner saved**: `getDocPath` throws "Owner
  document not saved" when the owner has no file name. That is what
  refused an import of a file with links (`docs/TransactionLog.md` sec
  35.2 item 6a).
- **The save form** is `<XLink file="..." stamp="..." name="..."
  [resolve="..."] .../>`. The log's pins and branch links (TransactionLog
  sec 27) added attributes the same way -- `version`, `vuuid`, `branch`
  -- "absent otherwise, and a FreeCAD that does not know them resolves
  the file". Older builds and upstream read `file` and ignore the rest.
- **Linked documents are keyed by their resolved full path**
  (`_DocInfoMap`), so whatever form a link saves, two links to one file
  share one `DocInfo` once resolved.
- **The document** already has map properties (`Meta`, `Material`,
  `App::PropertyMap`), saved in `Document.xml` with the rest.
- **Nothing** names a folder today. A library moved, or on another
  machine at another place, breaks every link into it that is not
  relative -- and a relative one breaks as soon as the document and the
  library do not move together.

## 3. Proposed shape

### 3.1 The system setting

`User parameter:BaseApp/Preferences/Document/LibraryPaths`: one string
per library, the name its key, the folder its value.

- A name is `[A-Za-z][A-Za-z0-9_]*`, compared case-sensitively.
- A folder is absolute, cleaned (`QDir::cleanPath`), `~` expanded when it
  is set; stored as entered, compared both as entered and canonical
  (3.5).
- Python: `App.getLibraryPaths()` -> `{name: folder}`,
  `App.setLibraryPath(name, folder)`, `App.removeLibraryPath(name)`.

### 3.2 The document's copy

`Document.LibraryPaths`, an `App::PropertyMap`: name -> the folder as it
was on the machine that last saved the document.

- **Only the names its links use**: an entry is added when a link takes
  the library form (3.4), and the unused ones are dropped at save.
- Editable by the user (property editor, Python): an edit is a remap,
  3.7.
- A property like any other: undoable, recorded by the transaction log,
  merged by it.

### 3.3 The save form

**Ruled (user, 2026-10-18), after a first ruling withdrawn:** the
library and the path under it are saved **beside** the old path, in every
schema:

    <XLink file="../../lib/fasteners/bolt.FCStd" lib="Lib"
           libfile="fasteners/bolt.FCStd" stamp="..." name="Bolt"/>

- `lib` and `libfile` are what this build reads first (3.6).
- `file` stays what the link's mode writes today, so an older build or
  upstream FreeCAD opens the document as it would have -- in a schema-4
  `.FCStd` saved for upstream, and in what `exportObjects` writes (copy,
  paste, drag across documents, `min(SaveSchemaVersion, 4)`) alike. A
  reader that does not know `lib` skips it, as it skips `version` and
  `branch` (TransactionLog sec 27).
- `Document.LibraryPaths` is written in both schemas: a reader that does
  not know a document property skips it.

The first ruling of the same day had the token in `file` itself in
schema 5 and the old path in schema 4. It was withdrawn for this: one
form in every schema, and a plain path kept to fall back to (3.6 step 3).

Everywhere a path is *shown* -- `getFilePath()`, the property editor, the
report view -- a library link reads in the token form,
`%Lib%/fasteners/bolt.FCStd`. Python may set a link with that string, and
`App.resolveLibraryPath("%Lib%/fasteners/bolt.FCStd")` gives the full
path. The token is the display and API form; the file keeps it as `lib`
and `libfile`.

### 3.4 When a link takes the library form

When a link's path is worked out (`getDocPath`, on a set and at a save),
its target's full path -- as given and canonical -- is matched against
the library folders:

1. **The candidates**: the document's map, then the system's. A target
   under a folder of the document's takes that name. A target under a
   system folder whose name the document has not got takes that name,
   and the document gets the entry.
2. **The deepest folder wins**: with `Lib` = `/lib` and `Std` =
   `/lib/std`, `/lib/std/bolt.FCStd` is `%Std%/bolt.FCStd`.
3. **One folder under two names**: the document's name if it has one,
   else the first by name; said once in the report view.
4. **A name the document maps elsewhere** -- the document has `Lib` =
   `/a/lib`, the system `Lib` = `/b/lib`, the target under `/b/lib` only:
   **the document gets a new name for the system's folder, `Lib_2`** (the
   next free `_<n>`), and the link takes it; said once in the report view
   (Q4, ruled). The document's `Lib` and the links using it are left as
   they are.
   **Not when the document's folder is not here** (found building step 3,
   2026-10-18; the user to confirm): a folder the document names that does
   not exist on this machine -- the library moved, the document saved on
   another machine -- is no clash. The link takes the name, as resolving
   it would (3.6 step 2), and the map follows (Q3). Without that, every
   library moved gave its next link a `Lib_2`.
5. **`libfile` never climbs**: a path with `..` after the folder is no
   library path.
6. **Modes**: `Dynamic`, `Relative` and `RelativeCanonical` take the
   library form; `Absolute` and `Canonical` are a user's explicit choice
   of an absolute path, and keep it (Q6).
7. **A document inside the same library** -- a part of the library
   linking another -- stays relative: the library is then
   self-contained, and moves or is renamed as a whole (Q5).

### 3.5 Matching

Paths are cleaned and compared as written and canonical, as `Dynamic`
does for symlinks; case-insensitively on Windows; a folder matches only
on a whole path component (`/lib` does not contain `/library/x`).

### 3.6 Resolving, on open (the ask: the document's first)

For `lib="N" libfile="F"`:

1. **The document's folder for N**, if `<folder>/F` exists.
2. **The system's folder for N**, if `<folder>/F` exists. This is the
   usual case on another machine: the document's folder is the path of
   the machine that saved it.
3. **`file`, as today**, by the link's mode: the document opened where
   nobody set up N still finds a library that moved with it.
4. **None**: the link is broken, and the report view names the library:
   "library 'Lib' is not set here (the document had /a/lib)". The Gui
   offers to set it (3.9).

**Both exist and differ** -- the document's `/a/lib` and the system's
`/b/lib` both hold `F`: the document's wins, as asked. The risk is that
`/a/lib` is an old copy and the user meant the current one, so it is
said once per document and name -- "Lib: the document uses /a/lib, the
settings say /b/lib" -- with an action, "use the settings' folder", that
remaps the document (3.7). Q2.

**What the document's map holds after a fallback**: nothing is written
at open -- opening does not modify. At the next save the map holds the
folder each name resolved through on this machine (Q3).

### 3.7 Changing a map

- **The document's entry edited**: the links using that name are
  resolved again -- detached from the file they had, attached to the new
  one, which is opened as a pending document is. One undo step, with
  the property.
- **The system setting changed** while documents are open (Q8, ruled):
  when the change is committed -- OK or Apply in the preferences, or
  `App.setLibraryPath` / `removeLibraryPath` -- the open documents are
  checked for links it would resolve differently: a name they use whose
  folder changed, was added where they had none, or was removed. If there
  are any, the Gui asks, listing the documents and the number of links
  in each: apply to them now -- each document's map remapped as in the
  item above, one undo step per document -- or leave them, to take the
  change when opened again. With no Gui (Python, headless) the documents
  are left, and the function returns what it would have changed.
- **A name renamed in the settings** renames nothing in documents: they
  keep the old name and resolve through their own folder, or say it is
  not set.

### 3.8 What else it touches

- **An owner not saved yet**: the library form needs no owner path, so a
  link into a library can be set and resolved from a new document. This
  also takes library links off TransactionLog sec 35 L1's list.
- **The transaction log** (TransactionLog sec 31.8, a value merged by
  what it holds): two copies of one assembly on two machines, the library
  at two places, write different `file` (and `stamp`) for one link while
  `lib` and `libfile` are the same. The link's value is compared by its
  library form when it has one, or every such link conflicts. To verify
  against the merge's comparison when built.
- **The export of a set** (TransactionLog sec 35 L7): library files are
  outside the top's folder. Two choices for the dialog: copy them under
  `_external/` as the design says, or leave the links in the library form
  for a recipient who has the same library.

### 3.9 The Gui

- **Preferences > General > Document**, a table "Library paths": name,
  folder, a browse button, add and remove.
- **The property editor** shows the document's `LibraryPaths`, and a
  link's path in the token form.
- **On open**, a library name that resolves nowhere is a notification
  with "Set library path...", which adds it to the settings and resolves
  the links again.

## 4. Existing documents

A link saved before this already points into a library by a relative or
an absolute path. It takes the library form when its path is next worked
out -- at load, in memory, without marking the document modified -- and
is written so at the next save. Q7, ruled.

The next save of a document opened from an older file is a new file:
since the merge with main, a plain Save of a schema-4 `.FCStd` writes the
current schema to a `.TStd` beside it and leaves the `.FCStd` as it was
(TransactionLog sec 27.62). The converted links go into that `.TStd`; the
old file keeps its old links. Save As in the standard format writes the
`.FCStd` with `lib` and `libfile` beside `file` (3.3).

## 5. Questions

| | Question | Proposed |
| --- | --- | --- |
| Q1 | The save form | **Ruled (user): `lib` + `libfile` beside `file`, in every schema** (3.3); a first ruling, the token in `file` in schema 5, withdrawn. |
| Q2 | Both folders hold the file and differ | **Ruled (user): the document's** (as asked), said once, with "use the settings' folder". |
| Q3 | The document's map after a fallback to the system's folder | **Ruled (user): rewritten at the next save** to the folder used here. The other way, never rewritten unless the user remaps, keeps the first machine's folder forever, and every other machine falls back. |
| Q4 | A name the document maps to another folder, the target under the system's | **Ruled (user): a new name in the document, `Lib_2`**, said. |
| Q5 | A document inside the library it links into | **Ruled (user): relative**, not the library form. |
| Q6 | `Absolute` and `Canonical` modes | **Ruled (user): kept absolute.** |
| Q7 | Links saved before this | **Ruled (user): converted at load in memory**, written at the next save -- for an old `.FCStd`, the `.TStd` a plain Save writes beside it (section 4). |
| Q8 | The system setting changed with documents open | **Ruled (user): checked when the change is committed, and asked** whether to apply it to the open documents it affects (3.7). |

## 6. Build order

1. **App**: the setting, `Document.LibraryPaths`, the save form
   (`lib` + `libfile` beside `file`), matching and resolution in
   `DocInfo::getDocPath` / `PropertyXLink::Restore`,
   the token in `getFilePath()` and in a Python `setValue`, the Python
   functions. Gtests: a link into a library saved and opened with the
   library moved (the system's folder), with only the document's, with
   neither, nested folders, the Q4 conflict, an unsaved owner, a
   document inside the library, an old document converted, a schema-4
   save opened again, a copy and paste across documents, the setting
   changed with documents open (what would change, returned).
2. **Gui**: the preference table, the check and the question when it is
   committed, the notification, the remap action.
3. **The log**: a link's value compared by its library form; a test of
   two copies with the library at two places merged without a conflict.
4. **The export option** of TransactionLog sec 35 L7, when that is built.

## 7. Built: step 1, App (2026-10-18)

`src/App/LibraryPaths.{h,cpp}`, `PropertyXLink` (`PropertyLinks.cpp`),
`Document.LibraryPaths`, the Python functions; gtests in
`tests/src/App/LibraryPaths.cpp`.

- **The setting**: `App::LibraryPaths::getSystem/setSystem/removeSystem`.
  The setters only write; a caller committing a change calls
  `changed(before)`, which emits `LibraryPaths::signalChanged(before,
  after)` -- for the Gui's question, step 2 -- and returns the open
  documents it affects with their link counts. A link is affected when the
  setting now resolves it (3.6 steps 1 and 2) to another file than it has;
  one the document's folder still holds is not (Q2). `apply(doc)` takes the
  setting's folders into the document for the names of those links, one
  undo step; `useSystemFolder(doc, name)` is the Q2 action.
- **Python**: `App.getLibraryPaths()`, `App.setLibraryPath(name, folder)`
  and `App.removeLibraryPath(name)` -> `{document name: links}` (what
  `App.applyLibraryPaths(doc)` would change; the documents are left),
  `App.resolveLibraryPath(token, doc=None)`.
- **The link** keeps the name, the path under the folder, and the folder it
  resolved through here (`getLibraryName/File/Folder()`). Its form is worked
  out where `DocInfo::get` finds the full path -- a set, a restore, a save
  of the linked file under a new name, a change of the resolve mode -- and
  kept when the link resolved through its own folder. `getFilePath()`, the
  shown path, is the token form. The C++ `setValue(file, name, subs)` takes
  a token. From Python, see section 10.
- **The map follows the links** (`LibraryPaths::settle`), as bookkeeping
  (no undo step, no remap): a name gets the folder its links resolved
  through here, in memory at the end of a restore (Q3; nothing is written)
  and again at each save, which also drops the names no link uses (3.2).
  A name its links resolved through two folders keeps the one the document
  names, and the links through the other are saved under a new name, as Q4
  does for a new link. A link that resolved through neither folder keeps
  its name and the document's folder for it, for a machine that has it.
- **A link resolved through its old path** to a file at its place under the
  library (the library moved with the document) takes that folder for the
  name.
- **An unsaved owner**: a link into a library is resolved at once, its old
  path the full one; the owner's first save writes it in the link's mode,
  and drops the library form if the owner was saved inside the library
  (Q5).
- **An edit of `Document.LibraryPaths`** remaps (3.7): the links whose name
  now resolves to another file are linked to it, in the same transaction
  as the edit; an undo puts both back.
- **Not changed**: a copy of objects with external links into a document
  never saved is refused as before ("Document must be saved at least once
  before link to external objects", `Document::copyObject`), library
  links or not. Once the target is saved, a copied library link resolves
  through the setting's folder and the target takes the name.
- **Gtests** (`LibraryPathsTest`, 17): names and tokens; matching (the
  deepest, the document's name, Q4, Q5, whole components); resolving (the
  document's first, Q2); saved beside the old path; opened with the
  library moved, with only the document's folder, with neither (broken,
  the library kept); nested folders; Q4 on a new link; an unsaved owner;
  an owner saved inside the library; an old document converted at load; a
  standard-format save opened again with the library moved; a copy into
  another document; an edit of the map as one undo step; a setting changed
  with a document open (what would change, then applied); a token.
- **Gates** (2026-10-18, `09c2c0f455`): the transaction log's 20 Gui checks
  pass; Python `-t 0` 3799 OK with the freeze on and off (52 and 54
  skipped); ctest gtest 979/979 both ways; the driven tests 288/291. The
  three are not this work's and came with the sync to `thundereal/main`,
  whose driven tests had not run on this tree:
  `RenderGoldenRaster` and `RenderGoldenRasterFlat` diverge in the beauty
  pass only (0.13% of pixels, max 26); `GuiTechDrawPageBeforeItsViewProviders`
  hangs on a "save changes?" dialog -- the test saves with
  `App.Document.saveAs`, which leaves the Gui document modified, then
  closes the page's subwindow, which holds the document's last view, so
  `Gui::Document::canClose` asks. Diagnosed, not compared against a build
  without this work.

## 8. Built: step 2, Gui (2026-10-18)

`src/Gui/LibraryPathsGui.{h,cpp}`, hooked up in `Gui::Application`'s
constructor (`LibraryPathsGui::init`); the driven test
`tests/gui/library-paths.py` (`GuiLibraryPaths_tests_run`).

- **Preferences > General > Document**, a group "Library paths" below the
  others: a table of name and folder with a "Browse..." per row, "Add" and
  "Remove". OK or Apply writes the setting -- a bad name or a folder that
  is not absolute is refused, with a warning listing them -- and commits
  the change (`LibraryPaths::changed`).
- **The question on a committed change** (Q8): the Gui listens to
  `LibraryPaths::signalChanged`, so a change from the page and one from
  `App.setLibraryPath` / `removeLibraryPath` are asked about alike. With
  open documents whose links the change resolves to other files, a
  question lists them with their link counts: "Apply now" applies to all
  of them (`LibraryPaths::apply`, one undo step each), "Leave them" leaves
  them to take it when opened again. `changed()` returns what the change
  affected before the question, so `App.setLibraryPath` returns the same
  with the Gui as without.
- **The report of an open** (3.9, Q2): once the documents of an open are
  up, a non-modal "Library paths" dialog lists what their library links
  met, one row per document and name, if anything (`LibraryPaths::
  problems`):
  - a name resolved through no folder -- "library 'Lib' is not set here
    (the document had /a/lib)", or the settings' folder does not hold the
    file -- with "Set library path...": a folder picked is set in the
    settings and applied to that document, and the other open documents
    it affects are asked about;
  - the document's folder taken where the settings' differs and holds the
    file too -- "the document uses /a/lib, the settings say /b/lib" --
    with "Use the settings' folder" (`LibraryPaths::useSystemFolder`).
- **The property editor** needs nothing of its own: `Document.LibraryPaths`
  is a map property of the document, and a link's file reads
  `getFilePath()`, the token form.
- **The test** covers the report of a Q2 conflict and its action, the
  question on a change from Python and "Apply now", and the page listing
  the setting. The "Set library path..." action runs a folder dialog, which
  the driven test does not; its effect is `setSystem` + `apply`, which the
  gtests cover.
- **Gates** (2026-10-18): the 20 Gui checks pass; Python 3799 OK with the
  freeze on and off; ctest gtest 979/979 both ways; the driven tests
  289/292, the new one passing and the three failing as in section 7.

## 9. Built: step 3, the log (2026-10-18)

**What was verified first.** A copy from another machine comes in as a
fork (TransactionLog sec 30.13): its rows are replayed here, against this
machine's settings, and the link values the log records are this
machine's -- the same text as a link set here, so a fork's and ours' link
to one library file already compare equal. With the library not set here
the replay stops at the row ("File ... does not exist") rather than record
a broken link. So the comparison the design asked for is not met through
a fork at all.

**Where it is met**: two branches of one file on one machine, each linking
the same library file through another folder -- two copies of a library,
or a library moved between the two. Both wrote the link (`lib` and
`libfile` the same, `file` and `stamp` not), and the merge called it a
conflict.

- **`LibraryPaths::comparableSaved`**: a saved value with the `file` and
  `stamp` of each `<XLink>` that has a `lib` left out.
- **The merge** (`planWeigh`, where both sides changed a property and
  ended differently) calls two values of an XLink type the same when they
  compare equal that way. Ours' value stays.
- **`Document.LibraryPaths` is kept, as `FileName` is** (`keptOnRestore`):
  a merge does not take theirs', a restore to a version does not put back
  an older one, a fork's import does not bring the other copy's. It says
  where the folders are on this machine, and the links settle it (3.2).
- **Tests**: `LibraryPathsTest.theMergeComparesALibraryLinkByItsLibrary`
  (gtest), `TransactionBranchCases.testLibraryLinksThroughTwoFoldersMergeAsTheSame`
  (Python) -- a conflict without the change, checked; "same" with it.
- **Gates** (2026-10-18): the Gui checks pass; Python 3800 OK with the
  freeze on and off; ctest gtest 980/980 both ways; the driven tests
  289/292, the three of section 7.

## 10. Built: a link set from Python by its file (2026-10-18)

Asked by the user the same day ("Agree, go"), the shape proposed: the
Python setters of `PropertyXLink` (and so `PropertyXLinkSub`) and
`PropertyXLinkSubList` (and so `PropertyXLinkList`) take

    obj.Link = ("%Lib%/f/bolt.FCStd", "Bolt")
    obj.Link = ("%Lib%/f/bolt.FCStd", "Bolt", "Face1")      # or a list of subnames
    obj.Links = [("%Lib%/f/bolt.FCStd", "Bolt", ["Face1"]), (other, "Edge1")]

- The first item a string tells the form from the old ones, whose first
  item is an object. It is a library token, an absolute path, or a path
  relative to the owner's file -- refused for an owner never saved, while a
  token needs no saved owner (3.8).
- The file is opened if no document has it, and the link is set to the
  object as any link is: the library form follows from where the file is
  (3.4), the same as for a link set to an object.
- Refused with `ValueError`: no library holding the token's file, no such
  file, no such object; `TypeError` for an object name that is not a
  string. In a list, an entry by file reports its own error.
- **Tests**: `Document.LibraryPathCases` (Python, 4), and
  `LibraryPathsTest.absoluteModesKeepNoLibraryForm` (gtest, Q6, which had
  no test of its own).
- **Gates** (2026-10-19): the Gui checks pass; Python 3804 OK with the
  freeze on and off; ctest gtest 981/981 both ways; the driven tests
  289/292, the three of section 7.
