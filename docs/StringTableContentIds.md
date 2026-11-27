# String ids from their content

Status: design agreed with the user 2026-11-18 ("Good. Write the doc and
start implementing"). Phase 1 built (sec 9), phase 2 built (sec 10), on by
default and readable by older builds (sec 11), on branch `Worker`. The
string table and the transaction log are also the remote session oplab's
area: every phase here is a separate commit for that review.

Companion to `docs/WorkerProcess.md` (sec 15 leased ids, sec 19 and 20,
where the problems below were found) and `docs/TransactionLogJournal.md`
(the log, which keeps the table's rows; cited as "log sec").

## 1. Why

An element's mapped name spells the ids of the strings it is built from
(`#1efaa;Wire#1efa8;:Hb42:b,W`), and names are compared as text. A string's
id is a counter of its table. So one content under two ids is two
different names for one element, and every way a table can come to give a
content a second id breaks a reference somewhere:

- a string nothing holds is freed, and made again later under a new id;
  the log keeps one content under two ids because of it (log sec 27.41 Q2,
  the table is a multiset);
- another process's table, or another document's, gives the same content
  its own id: a worker's result is refused ("string 131105 is held here as
  772", WorkerProcess sec 19-20) and read by content, every name rewritten;
- a copy of a table that differs in any way -- what it holds, its naming
  mode (WorkerProcess sec 20), how a string is split -- makes the content
  again, and every name built on it differs: Binder032's wire.

Leased id blocks (WorkerProcess sec 15) keep two processes from giving the
same id to different strings; nothing keeps them from giving one string two
ids.

## 2. The table today

`App::StringHasher`, src/App/StringHasher.{h,cpp}:

- `StringID`: a `long` id (32 bits on Windows), `_data`, `_postfix`, flags,
  and `_sids`, the strings it refers to. Spelled `#<hex>` or
  `#<hex>:<hex index>` (`StringID::toString`).
- A reference is either **structural** -- the whole prefix is another
  string (`PrefixID`, `PrefixIDIndex`, `Indexed`) or the whole postfix is
  (`PostfixEncoded`) -- or an id **spelled inside free text**, as the
  `#387` of the postfix `;:G#387;FUS;:H-4f7:b,E` (a postfix with a `#` is
  kept as text, `getID`, StringHasher.cpp:1001). Both are in `_sids`.
- Saved (`saveStream`) with embedded ids, not flattened: each row is its
  id, its flags, the ids of `_sids`, and only the text that is not a
  structural reference.
- Looked up by its bytes (`StringIDHasher`: data and postfix), ids spelled
  in them included: identity depends on the spelling of every id inside.
- One string has **two in-memory forms**: made in a session, an encoded
  postfix holds `#<id>`; read back from a stream it holds the referenced
  string's text (`restoreStreamNew`, `d._postfix = d._sids[0]._sid->_data`).
  `getID` looks both up (StringHasher.cpp:985 and :1100).

## 3. The design

### 3.1 Identity: a digest

Each string has a 128-bit **digest** of its canonical content, BLAKE2s-128
(RFC 7693, `QCryptographicHash::Blake2s_128`; Python's
`hashlib.blake2s(digest_size=16)` gives the same, which the tests use). The
canonical content (version 1) is the bytes

    "FCSID1" 0x00
    kind      1 byte: Binary (1) | Hashed (2) of the flags
    data      u32 length, then the canonical data
    postfix   u32 length, then the canonical postfix

lengths little-endian, where

- the canonical data of a string whose prefix is structural (`Indexed`,
  `PrefixID`, `PrefixIDIndex`) is 0x01 and the referenced string's digest,
  then for `PrefixIDIndex` the `:` that follows it; of a binary or hashed
  string its bytes; else its text with every id spelled inside it
  replaced (below);
- the canonical postfix of a `PostfixEncoded` string is 0x01 and the
  referenced string's digest; else its text with ids replaced;
- an id spelled inside text is `#` and hex digits naming a string in
  `_sids`: replaced by 0x01 and that string's digest. A `#` naming no
  string of `_sids` (another table's, or no id) is left as it is.

A referenced string's digest is a field of it, made when it was: a digest
is one pass over the string's own bytes, no recursion, no table lookup.
Both in-memory forms (sec 2) give the same digest; so do two tables that
gave the same content different ids. Strings a string holds that are not
in its text (`_sids` beyond its references) are history kept alive, not
identity: today's lookup ignores them too.

Measured (mini, 2026-11-18): BLAKE2s-128 of a 33-byte name 133-152 ns,
`qHash` 10-12 ns. A digest is made only for a new string: scanner.FCStd's
recompute makes 11,593 (40,783 to 52,376 strings in 13.8 s), 1.6 ms of
digests; reading its 40,783-string table, about 6 ms. The table's whole
share of that recompute, sampled, is about 10 ms.

### 3.2 The spelled id: a short prefix of it

A name spells a string by a **prefix** of its digest: 32 bits by default,
7 characters of base 32, under a marker other than `#` so that a name of
an older file reads as it did. Levels: 32, 64, 128 bits.

The marker: across 505,495 element names (scanner.FCStd and the tree's
PartDesign and example files) the only punctuation used is `# ( ) , - :
; @ |`. Of the rest, `$` begins a label in a subname, `+ - * / ^ % < > = !
&` are an expression's, `< > &` an XML attribute's: `~` is the candidate
(or `` ` ``, `{`), to be checked against the subname and expression
grammars before phase 2 fixes it.

**The registry.** The table keeps, append-only, one entry per contested
prefix: the prefix, its level, and its owner's canonical content. A string
is spelled at the first level whose prefix has no entry, or whose entry's
owner is its content. Entries are never removed: a string freed and made
again is spelled as it was, whatever else came meanwhile.

Expected entries: N^2/2 / 2^32 -- 0.2 for scanner's 40,000 strings, 116 for
a million. Almost every string is spelled at 32 bits, which is about
today's length (`#23ef5` is 6 characters) and fits today's `long`.

### 3.3 Collisions are found, never assumed away

A lookup that finds a digest, or a prefix, compares the content (canonical
form, references by digest) -- as today's lookup compares bytes after the
hash matched. So a collision at any level is found the moment the second
string comes into a table, and two contents meet only in a table: a
worker's result put in, a branch merged, a string imported from another
document. The rule is one at every level:

- in one table, the string there first keeps the spelling; the other
  moves up a level, the registry recording the owner by content;
- where tables meet, the authority's (the document's, the branch merged
  into) keeps it; the incoming string moves up, and the incoming names
  that spell it -- and those of incoming strings built on it, whose
  digests mix its own -- are rewritten as they come in: the by-content
  import (`RestoreStrings`) narrowed to that family.

A collision of full 128-bit digests (about 1.5e-27 for a million strings)
is handled the same way: the second content gets the digest of its content
and a counter, its owner recorded by content.

A worker that makes a string the client made meanwhile, with the same
32-bit prefix and another content: 5,000 new in the job, 100 here, about
1 in 10,000 jobs.

## 4. What changes, and for whom

- **Ids become `int64_t`** where they are held (`StringID::_id`, `IndexID`,
  rows, the log's columns, Python): the 32-bit level fits a `long`, the
  higher ones do not.
- **The table's order.** Saved rows go in creation order (a string after
  every string it refers to), not id order: a sequence number per string.
  The log's "rows past the highest written" (`rows(after)`, `LowTaken`,
  WorkerProcess sec 15) go by that sequence -- the log's part (oplab).
- **Workers**: no leases; a result's strings come back as they are, the
  check (`takeRows`) only asking that equal spellings be equal content.
- **Merges and imports** (`merge`, `importID`, `RestoreStrings`): identity
  across tables is the digest; a rewrite only where sec 3.3 moved a string.
- **Compaction** drops unheld strings; the ids of those kept do not change.
- **Hashed strings** (`Hashed`, the SHA-1 of a long name): the SHA-1 must
  be taken over the canonical text, or an id inside it that moved up a
  level would change the hashed string's identity.
- **The naming mode** (`IndexedNames`) stays the document's: it decides how
  a name is split, so which strings there are.

## 5. Old files

Counter ids stay valid under `#`. On open, a schema bump rewrites a file's
table to spelled digests and every name and reference with it -- the
by-content import, once (about the 3 s of the GUI's by-content put-in on
scanner; to measure). Read-only uses can keep the old ids: a process that
holds the old table finds them by content.

## 6. Python and diagnostics

`StringID.Digest` (32 hex characters); `StringHasher.duplicates()`: the
groups of ids of one table whose contents are one (same digest) -- what
sec 1 counts.

## 7. Tests

- Two tables building one content in different orders, under different
  ids: equal digests, derived strings included.
- A table saved and read back: every digest as before (both forms, sec 2).
- The digest of plain, postfixed, encoded and free-text-id strings checked
  against Python's `hashlib.blake2s` over sec 3.1's bytes: the format pinned.
- Later phases: a forced collision (a test hook shortening the prefix)
  through each rule of sec 3.3; a worker's result with no leases; an old
  file migrated, its references holding.

## 8. Phases

1. **Identity** (this commit): the digest of every string, computed where
   strings are put in (`StringHasher::insert`), the Python attribute and
   `duplicates()`, the tests. No format change, no change of behaviour.
   Measured with it: the duplicates of scanner's table, and of a worker's
   copy.
2. **Spelled digests for new strings**, behind a schema bump: `int64_t`
   ids, the marker, the registry, creation order, verified lookups.
3. **The log** by creation sequence (oplab).
4. **Workers** without leases; conflicts by sec 3.3.
5. **Old files** migrated on open.

## 9. Phase 1, as built (2026-11-18)

- `StringID::digest()` / `digestText()`, made in `StringHasher::insert()`
  -- every way a string comes into a table goes through it: `getID`, the
  two stream readers, `insertRows`, `takeRows`, `merge`, `importID`. The
  strings a string refers to are in the table before it, their digests
  made. `StringHasher::duplicates()`; Python `StringID.Digest`,
  `StringHasher.duplicates()`.
- Tests (tests/src/App/StringHasher.cpp): `digestIsPinnedToItsFormat`
  (against `hashlib.blake2s`), `oneContentOneDigestUnderOtherIds` (two
  tables, other ids, both naming modes, a name built on another by its
  spelled id), `aTableReadBackKeepsEveryDigest` (both in-memory forms),
  `duplicatesFindsOneContentUnderTwoIds` (another table's rows taken in).
- Cost: scanner.FCStd opens in 2.67 s as before, its 40,783 strings
  digested.

Measured with it, scanner.FCStd:

- No table holds one content twice: after the open, after a recompute
  here, in a worker's copy before and after its recompute -- 0 groups.
  The duplicates of sec 1 are between tables, and across time.
- Binder032 (WorkerProcess sec 20) is the second: after a worker's result
  is put in, the strings of its wire's old name -- `#1efaa`
  `Edge989;:Hb42,E`, `#1efa8` the edge pair -- are freed, and the geometry
  search that re-resolves `Pad050.Profile` makes them again as `#22b1d`
  and `#22b1b`: digests `7679bace5287...` and `279341f51669...`, those of
  the freed ones. One content, a new id, a new name for the wire. A
  recompute here keeps them held. With spelled digests (phase 2) it gets
  its old spelling back, and `Pad050`'s name still resolves.

## 10. Phase 2, as built (2026-11-18): spelled digests, behind a mode

What sec 4 planned changed on one point, for the better: **a string keeps
its counted id** -- the table's key, in creation order -- and only what a
name spells changes. `StringID::spelled()` is the number after `#`: the id
in a table of counted ids, the digest's prefix in one that spells digests.
Every saved list of ids, the log's rows and their order (`rows(after)`, the
lowest taken, the leases), the element maps' lists, stay as they were; the
log needed nothing. One content made under two ids in one table is two
entries spelled alike: names that read the same, a duplicate that costs
memory, not a reference.

- **The mode**: `StringHasher::setContentIds()`, Python `ContentIds`; a new
  table takes Preferences/Document `ContentStringIds` (on since sec 11); saved
  as `contentids="1"` on the table's element (`Save`, `saveReference`), and
  in a table file with the registry (first as a `v2` header; since sec 11
  after the rows, a v1 table an older build reads). Restored without it, a
  table counts ids, as every table before did.
- **The spelling** (sec 3.2): level 0 the digest's first 32 bits; above,
  52 bits of the digest of the digest and the level, under bit 60 and the
  level. The registry (`spellingRegistry()`, `takeRegistry()`) records a
  contest with its owner's digest, saved with the table; a spelling index
  finds a string by the number its names spell (`getIDBySpelling()`, Python
  `getIDBySpelling()`, `StringID.Spelled`).
- **Numbers read from a name's text are spellings**, numbers from a saved
  list are ids -- the rule every site was sorted by:
  `getID(IndexID)` (a parsed id) looks up by spelling; `toString()` writes
  the spelling; switched: the element map's prefix-id names and its
  legacy-crossing check, `GeoFeature`'s ids of a name, `TopoShape`'s
  postfix ids and combo names, `ShapeDiff`, the table's own import
  (`rewriteIds`, `importNew`, `mintedReference`), `getID(MappedName)`'s
  prefix match, the digest's own scan. In a table of counted ids every one
  is what it was. `IndexID::id` is 64 bits.
- **Workers**: the setup carries `contentids` and the registry, the copy's
  tables taking them before they spell (as `indexed`, WorkerProcess
  sec 20).
- **Names in order of age.** `ElementNameComp` orders names that lead with
  `#` by the number, which in a table of counted ids is a string's age, and
  naming picks the first: a face is named by the oldest of its edges' names.
  Digests give no age, and a PartDesign test showed it -- a sketch on a
  pad's top face lost it when an edge added later sorted first. While an
  `ElementNameOrder` names a table that spells digests, the order goes by
  the ids of the strings spelled (`TopoShape::makESHAPE`,
  `FaceMaker::postBuild` set it): the oldest first again. Two processes give
  their new strings ids in the same order, a worker's from its lease above
  the client's.

Tests: `contentIdsSpellOneContentAlikeInTwoTables`,
`aTableOfCountedIdsSpellsItsIds`, `contestedSpellingsMoveUpAndStay` (a
4-bit first level: contests move up, a table read back and one rebuilt in
another order with the registry spell as before),
`namesSpellingDigestsSortByTheirStringsAge`. The Python suites, run with
`ContentStringIds` on, every new document spelling digests: Part,
PartDesign, Sketcher, WorkerPool, Animation as with it off; Document's
failures were its own helpers reading a name's numbers as ids
(`spelledIds()`, `getIDBySpelling`), and two tests that asserted a string
made again gets a new name -- which digests are there to prevent.

Next:

- a worker's rows whose content the client holds under other ids are taken
  in as they come (they spell alike): no read by content;
- the mode on for new documents, behind the schema;
- old files migrated on open (sec 5).



## 11. On by default, and a file an older build still reads (2026-11-18)

The user: "If integer id is saved, then i think it is possible to make the
table stores in backward compatible mode. And we can amend the name map
version to trigger migration recompute in older version software. If so,
make it default on."

- **The table stays a v1 table.** Its rows are what they were -- by
  counted id -- and the mode and the registry come after them:
  `StringTableContent <n>` and the entries. An older build reads its count
  of rows and stops; this one, finding the trailer, takes the registry and
  spells what it read by digests, in the order the strings were made. On the
  table's element they are attributes (`contentids`, `registry`), which an
  older build ignores. The `v2` header of sec 10 is gone.
- **The element maps' version** says it: `ComplexGeoData` gives `4.C` where
  its table spells digests, `4` elsewhere. An older build knows `3`, `4`,
  `3.*`, finds `...4.C`, and asks for the migration recompute that makes
  every name again with its own counted ids -- the path any naming-version
  change takes (`PropertyPartShape::Restore`, `addRecomputeObject`). This
  build expects `4.C` exactly where the table spells digests, so an older
  file keeps matching `4` and is left as it was.
- **On by default**: Preferences/Document `ContentStringIds` (true) for a
  new document. A table restored without the mode counts ids; sec 12 says
  when an old file changes.

Checked with an older packaged build (dist-test env-1102, build 14555):
scanner.FCStd saved both ways by this build opens in it; for the one
spelling digests it reports `version change ... 1.17.80001.4.C ->
1.15.80001.4` and the missing prefix ids, and after its recompute both
come out the same -- 501,825 names, the same 13 objects invalid (that build
lacks what this one has; it sees 456 of 686 objects).

Benchmark, mini, scanner.FCStd made both ways by this build (opened,
every object recomputed, saved; then each measured twice: open, recompute
everything, save):

| | counted ids | digests |
|---|---|---|
| open | 1.37 s | 1.33 s |
| recompute everything (627) | 10.36-10.38 s | 10.39 s |
| save | 1.82-1.83 s | 1.80 s |
| memory after the open | 280-282 MB | 263-266 MB |
| peak memory | 984-1064 MB | 1029-1035 MB |
| strings in the table (after a recompute) | 51,985 (52,443) | 21,305 (21,800) |
| element names | 501,909 | 501,909 |
| average name | 45.4 characters | 46.6 characters |
| file | 7.68 MB | 7.78 MB (+1.3%) |
| string table member | 137 KB packed, 1021 KB raw | 95 KB, 530 KB |
| Document.xml | 470 KB | 543 KB |

Speed is the same within noise. The table holds 59% fewer strings: a
string made again with the same content now has the same bytes, and the
table finds it instead of making another. The file is 1.3% larger: the
names, an id of 8 hex digits where counted ids reach 5, are a little longer
in the element maps and the references (Document.xml +73 KB), more than the
smaller table saves (-42 KB).

## 12. An old file converts when its migration is accepted (2026-11-18)

Sec 11 first converted an old file as it was read, behind a preference
that was off. Now an old file opens counting its ids, as it was saved, and
nothing about it changes on the open. It converts at one moment only: the
migration prompt ("Some document(s) require recomputation for migration
purpose ...", `Gui::Application`, at `signalFinishOpenDocument`), when its
user answers Yes. Every name of the file is made again then anyway, so the
conversion costs nothing more than the recompute already asked for. A file
whose names need no migration never sees the prompt and keeps counting;
one whose user answers No keeps counting too.

- **`Document::spellNamesByContent()`** (Python `doc.spellNamesByContent()`)
  switches the document's table (`StringHasher::setContentIds(true)`: every
  string it holds spelled by its digest, the registry consulted) and marks
  for the recompute every object with a shape that names elements with that
  table -- `enforceRecompute()` and `MigrationPending`, as a restore marks
  an object whose names are out of date. All of them, not only the ones
  the restore marked: a name made under the counted table spells a handle,
  which the switched table would read as a digest's spelling. It returns
  the number marked, and does nothing (0) for a table spelling digests
  already, or for a document without one.
- **The prompt** calls it for each document that asked for the migration,
  before the recompute, worker or not: a worker given the document takes the
  mode with it (`setup["contentids"]`, sec 10). Documents recomputed only
  because they depend on those are not converted.
- **Preferences/Document `ContentStringIdsForOldFiles`** now gates this,
  and is on (true). Off, an accepted migration recomputes the file counting
  ids, as before sec 11. The restore no longer reads it.
- Not on the console: `FreeCADCmd` shows no prompt and converts nothing;
  a script calls `doc.spellNamesByContent()` and `doc.recompute()`.

Two places took a table's mode from the hasher's constructor, that is
from `ContentStringIds`, which is right only for a new document; both now
take it from the file:

- **The file's hasher, filled from the transaction log** (sec 27.40 item 2,
  `shared` in `Document::Restore`): an open in the GUI, where the file's log
  holds its strings, joins the table it reads into that hasher, made
  spelling digests. The merge left the mode as it was, so an old file came
  back spelling digests, its names `4` against `4.C` expected: the
  migration prompt at every open of an old file in the GUI, and a recompute
  of everything named if accepted (seen at the GUI test's first open; the
  console, without the log's strings, did not show it). The file's table now
  sets the shared hasher's mode, as it sets a hasher of the document's own.
- **`StringHasher::restoreTable()`** of a table without the trailer, or of
  an older private table, now counts ids whatever the hasher was made
  spelling; with the trailer it spells digests, as before. A table read on
  its own (`FileHistory::readTable`, the workers) is told its mode by its
  bytes alone.

GUI test `tests/gui/migration-spells-digests.py` (12 claims): the prompt
answered No leaves the table counting and the names as saved; answered Yes
the table spells digests, the recompute is done, and the cut names the same
44 elements by spellings the table holds.

A shape whose recompute fails keeps the names it had, spelling handles under
a table that spells digests; a reference to one of them looks the element
up by geometry, as any reference to a name the migration could not make
again does.

Test: `TransactionBranchCases.testAnAcceptedMigrationSpellsAnOldFileByDigests`
-- a cut of two boxes saved counting ids reopens counting them, untouched,
its names as saved; `spellNamesByContent()` marks the cut alone (the boxes
name their elements without the table), a second call marks none; after the recompute the cut names the same elements
by digests, and saved and reopened it reads them as they are, untouched.

## 13. What a 3D view put in the table before an open (2026-11-18)

In the GUI a worker's result for scanner.FCStd went in by content, "string
131105 is held here as 772" (docs/WorkerProcess.md sec 20). The worker's
copy of the table held 151 strings; this process held 41,048.

- **Why.** The GUI makes a document's 3D view when the document is made,
  before an open reads the file. The view materialises its `Render_*`
  overrides as dynamic properties (`_containerProperty`,
  `View3DInventorViewer.cpp`), and a dynamic property's documentation goes
  into its owner document's string table (`DynamicProperty`,
  `docID`): 104 strings, ids 1 to 104, before the file's table is read.
  The open found the file's hasher (the transaction log's, sec 27.40 item
  2) not empty, merged the file's table into it, found ids 1 to 104 meaning
  other strings there, and kept the document's table apart. The log stores
  the strings of the file's hasher, which then held the 104 and what came
  after (151), and a worker reads its copy from the log. Not seen on the
  console, which makes no view; not seen with a file the GUI made, whose
  own view put the same texts at the same ids.
- **Fix.** A dynamic property's documentation goes in the document's table
  for the document itself and for its objects and view providers
  (`TransactionalObject`) only; any other container of the document -- a
  3D view -- keeps the text (`documentationTable()`, `DynamicProperty.cpp`).
  What such a view saves is the text, which restore reads as before; a
  `docID` a view saved before still resolves.
- **Measured** (mini, GUI, scanner.FCStd, recompute everything by a
  worker): the worker's copy 41,043 strings; `ById` true; put-in 2.34 s
  against 3.01 s. The shapes as before: 3 of 686 objects differ from a
  recompute here (two infinite planes by volume noise, Binder032).

Test: `tests/gui/worker-table-after-open.py` (7 claims) -- a cut made with
no view, saved, opened in the GUI: a worker holds every string its names
spell, as here, and its recompute goes in by id. Without the fix the
worker's copy holds 107 strings and none of the 6.

## 14. A worker's result emptied the document's table (2026-11-19)

With sec 13 in, the GUI comparison of a recompute here and by a worker
still had Binder032 (49 names here, 50 from the worker), and on the copy
that spells digests Hole004 and Sketch046 too. Their names, expanded, were
not the problem: after the put-in the document's string table held 139
strings. On the console, scanner.FCStd, everything touched and recomputed by
a worker: the table went from 51,683 strings to 34 at the 97th object put in
(Sketch018), and 207 objects' names spelled 26,508 ids it no longer held. A
save wrote them so, and the file reopened kept the loss.

- **Why.** A shape's value says which string table its element map is in
  by `HasherIndex`, and with `SaveHasher` carries that table. Both are set
  by `PropertyPartShape::beforeSave()` for the save under way, which numbers
  the document's tables with the document's first (`Document::Save`). An
  export (`exportObjects`, so `copyObject`) numbers them afresh and does not
  put the document's first: a shape on the document's table registers it as
  new, index 0, to go with the value. `SubShapeBinder` with
  `BindCopyOnChange` 2 copies what it binds on every recompute, so in the
  worker the bound objects' shapes were left saying "index 0, the table
  with it". The worker captures every value after its recompute
  (`captureValue`), and a capture is no save: `Save()` wrote the stale pair,
  and the table that went with it -- the strings marked in the worker,
  which had saved nothing: 34. Here index 0 is the document's table, and
  `PropertyPartShape::Restore()` read the carried one into it with
  `StringHasher::Restore()`, which clears first.
- **The log was not hit**: it captures a detached copy of a property, with
  no owner, which writes no index at all.
- **Fix, both ends.** A capture (`App::capturingDocument()`) settles the
  pair itself: index 0, and the table goes with it only when it is not the
  document's. A value restored (`App::restoringValue()`, new: true inside
  `restoreValue()`) never reads a table into one the document has: the
  document's table, or the one it carries read into a table of its own.
  Its index is the writer's numbering, not this document's.
- **Measured** (console, scanner.FCStd, everything touched, recomputed here
  and by a worker, each object's names compared by their content expanded
  to the end): table 51,685 strings after the put-in; no name of any
  object spelling an id the table lacks, before or after a save and reopen;
  0 of 416 objects with names differing. On the copy spelling digests the
  names are the same bytes, 416 of 416. Binder032's extra name was this:
  49 both ways now.

Test: `TestWorkerRecompute.testAMutatedBinderLeavesTheTableWhole` -- a cut
of a box and a cylinder bound by a Mutated binder, the box changed, the
recompute given to a worker: the table keeps its strings and every id the
cut's and the binder's names spell is in it. Without the fix the table
went from 8 strings to 3.


## 15. One content held once: the other ids of a string (2026-11-20)

Asked 2026-11-20: is the counted id still needed, and can it go. Kept: it
is the table's key in every saved list and in the log, and it is the age
names are ordered by (sec 10), which a digest cannot say. What it left
behind in a table that spells digests was one content under two ids
(sec 10: "a duplicate that costs memory, not a reference"), and that was
measured first.

- **Where it happens.** Not in a recompute by a worker: scanner (the copy
  spelling digests, 21,227 strings), everything recomputed here and then by
  a worker, eight edits recomputed by a worker, objects made here while a
  worker ran -- no content under two ids at any point, nor after a save and
  a reopen. In the Animation, when two workers each make the frames whose
  topology the version never had: a plate with 64 holes, its height keyed
  past the tools so the holes go blind, 61 frames by two workers -- each
  gave the 256 new strings an id of its own lease, and the table held 835
  strings, 256 of them twice. A save wrote both and a reopen read both.
- **Now: the second id is another id of the entry.** In a table that
  spells digests, a row coming in (`insertRows`, so `takeRows` and the
  log's `loadStrings`; `merge`) whose content is held already adds no
  entry. `insertOrAlias` finds the entry of the same digest and records
  the id as another id of it (`aliases()`, Python `aliases()`): `getID(id)`
  gives the entry, `hasID` says yes, and the id is never given again
  (`lastID`). The same plate: 579 strings, 256 other ids.
- **The entry takes the lower id.** The lower id is the older string, and
  names are ordered by age (`ElementNameOrder`): `getIDBySpelling` gave the
  lowest of the ids of one content before, and gives the entry's now. When
  the row coming in has the lower id the entry is moved to it and its old
  id becomes the other one; a reference held to the entry follows, being
  the same object.
- **As rows, every id.** `rows()` gives the other ids as rows of their own,
  in id order, and `rowsOf()` gives a row under the id asked for: the log's
  store, and a worker filled from it, read whatever was written under
  either id. A table filled from those rows comes to the same entries and
  the same other ids.
- **A save writes the entry alone.** `saveStream` writes entries, under the
  id each is held by. `compact()` -- a save's -- lets go the other ids that
  nothing kept uses (`keep`, what the log's retained values name), and an
  entry stays while one of its other ids is kept. A frame of the Animation
  carries its rows and takes them in at every read, so its ids are read
  again as other ids after that. The plate after a save: 582 strings, no
  other id; reopened, the same.
- **A table of counted ids is as it was**: there the id is the spelling,
  two ids are two names, `takeRows` refuses and `insertRows` takes both.
- **Whether a row is the string held** (`isRow`) is said by the entries its
  ids name, not the numbers, and not by whether it is kept for good
  (`Persistent`): an id may be another id of what the entry refers to.

Tests (`StringHasherTest`, 74): `aContentComingInUnderAnotherIdIsHeldOnce`,
`anEntryTakesTheLowerOfItsIds`,
`theOtherIdsOfAStringGoAsRowsAndNotIntoASave`,
`compactLetsGoTheOtherIdsNothingKeptUses`;
`rowsOfAContentHeldUnderOtherIdsComeInSpelledAlike` now expects one entry.
A table whose rows are not in the order their references need is
docs/WorkerProcess.md sec 32.
