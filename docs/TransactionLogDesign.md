# Transaction log: design

Every FreeCAD document of this fork keeps a history: an append-only log of
every committed transaction, held per file in SQLite and, by default, written
into the file at each save. Undo is a new forward transaction that applies the
inverse of an earlier one, so undo, redo, restore, merge and recovery are all
ordinary rows of the same log. On top of the rows sit versions (immutable
points), branches (movable tips), a three-way merge that looks inside values,
pinned links to versions of other files, crash recovery that replays the log,
two processes saving one file, documents saved as folders, linked sets of files
brought in as branches, object UIDs, a Gui panel and a browser History panel.
This document states the design as it stands and says only briefly how it is
built.

## 0. How to read this

- This is the design. The build record, with every survey, measurement,
  probe and reversal, is `docs/TransactionLogJournal.md` (the implementation
  journal). The user manual is `docs/TransactionLogGuide.md`.
- "Journal: 27.22, 30.10" points into the journal; find a number with
  `grep -n '^### 27.22\|^## 30' docs/TransactionLogJournal.md`. Code comments
  cite the same numbers ("sec 27.22").
- "(user, YYYY-MM-DD)" marks a binding ruling of the user. A later ruling
  replaces an earlier one; only the one that stands is given, with one line
  where a reversal explains the rule.
- Sec 39 (the audit of 2026-10-27) is folded into the rules where it applies.

## 1. Purpose and principles

**The idea.** Follow Onshape and git: the document's history is an append-only
log; undo is not a pop but a new transaction carrying the inverse of an earlier
one; the log is persisted and travels with the file. Undo stops being a mode
and becomes an edit, so it can be recorded, replayed, streamed and attributed.
Git is the analogy; the store is SQLite.

**The additive rule** (user, 2026-09-21). The document model is untouched: the
old `Transaction` (object create, object remove, property set, dynamic property
add/remove) stays the unit, and the log implements its `Save`/`Restore` against
a store. A document whose user never opens the panel sees the same undo, the
same files and the same speed; an upstream FreeCAD reading the file round-trips
the history members (`History`, `Version`, `Branch`) as plain data.

**What it is for.**
- The undo/redo system and the autosave/crash-recovery system. It is not a
  setting (user, 2026-09-22; "the log is always on", user, 2026-09-26).
- The substrate for several clients of one document (a shared head, authors,
  per-author undo) and for the browser tier (the History panel is queries over
  the store).
- Versions, branches and merge for CAD files, at property level; pinned links
  to versions of other files; forks of a file brought back as branches.

**What it is not.**
- Not operation-level: a row is property changes, mechanical and complete,
  needing no feature to cooperate (Figma ships multiplayer on the same).
  Operations may come later as delta encodings inside the property log, not as
  a second log.
- Not operational transformation or CRDT: a step whose effects were overwritten
  is refused with reasons, never transformed (Onshape's behaviour); CRDT
  tombstones never collect.
- Not a live store shared by processes: two processes meet at the file, with a
  pull in every save.
- Not a record of view state: camera and display modes are not logged; Python
  proxy state is never logged (user, 2026-10-26).

**Principles that decide most rules.**
- One property on one object must not get slower: serialisation runs on a
  worker thread, from copies the undo system already made.
- Values are content-addressed: undo and redo store nothing new, "can this
  still be undone" is a hash comparison, and truncation is safe because every
  op carries both before and after.
- A value a recompute wrote is a cache: it may go missing, never stale.
- A property's value changes only through the property; a write that escapes
  `aboutToSetValue` is a defect fixed at the site (user, 2026-09-23).
- Everything inside the log is schema 5 (user, 2026-09-28).
- Every new record shows in the panel; the panel is the first verifier.

Journal: 1-8, 18, 19, 22.

## 2. The record

### 2.1 Rows and ops

A *row* (store table `txn`) is one committed transaction. It groups *ops*; an
op names a container and a property and carries a before and an after value
reference.

| Op | Target | Payload |
|---|---|---|
| `create` | object id, name, type | one `set` per persisted property and an `addprop` per dynamic one; meta `{"uid", "ext"}` |
| `remove` | object id, name, type | one resolved `set` (before only) per property, the removed dynamic properties with metadata, then the op |
| `set` | container, property | before and after refs, `derived` flag, `touched` bits |
| `addprop` / `delprop` | container, property | type, group, doc, attributes, dynamic status bits (+ before ref for `delprop`) |

- A container is `doc`, `obj:<id>` or `view:<id>` (a view provider, logged
  under its owner's id). Objects are named by `DocumentObject::getID()`: per
  file, persisted, never reused. Rename is a `Label` set.
- Replay rule: a property's state is the newest non-empty ref in op order; an
  object exists between its `create` and `remove`. An object made and removed
  in one transaction leaves no ops; a set whose before and after hash equal is
  dropped.
- Resolution is the transaction: only the value before the first write and
  after the last matter (a sketch drag writes `Geometry` hundreds of times and
  is serialised once, at commit).
- A row removing a dynamic property and adding one of the same name emits its
  removals first; a cold undo of it puts the removed property back with its
  value (sec 39).

**Row fields.** `seq` (per store, one counter for all branches), `parent` (the
branch head at append; the log is a tree), `merge_from` (second parent of a
merge), `branch`, `kind`, `origin`, `name`, `time`, `session`, `ord`, `inverts`
(the row an undo/redo inverts), `script` (a tagged JSON annotation). A row is
known across copies of the file by its identity `(session.uuid, ord)` (section
10.3), never by `seq`.

**Row numbering.** A row is always numbered after the row it follows; nothing is
reparented to a lower number (walks rely on `chain(head, N+1)`). A number
removed at the tip (a rolled-back merge, a deleted branch) stays taken: the
store keeps the highest given in meta `seq_counter` (sec 39).

**Kinds.**

| Kind | Ops | Meaning |
|---|---|---|
| user, implicit | yes | an edit; implicit = opened by a write with no transaction open |
| undo, redo | yes | the inverse of `inverts` |
| recompute | no | record written right after the row it ran in: per object touched state before/after, error, seconds |
| save, snapshot | no | a version was recorded |
| restore | yes | the document made equal to a version, or an open's file as found |
| switch, branch | no | branch arrived on / made |
| trim, squash | squash: yes | rows removed / rows replaced by their net change |
| merge | usually | a merge; with no ops it is a record that moves the base |
| pick | yes | rows of another branch applied by hand |
| import, request, drop, set | no | a fork brought in, a sent file arrived/let go, a linked set's progress |
| login | no | a connection admitted to a shared session |
| recover | no | a crash recovery: source, anchor, rows replayed, rows skipped |

Records (rows with no ops) are not movement: they never count as "ahead",
never make an undo step, and a fast-forward re-appends them.

**Implicit transactions** (user, 2026-09-22). A write with no transaction open
opens one flagged with its origin (`python`, `gui`, `recompute`, `restore`),
closed when the *invocation* that opened it returns (console line, macro,
script, recompute; `Application::InvocationScope`), not an event-loop turn, so
the rule is the same in the GUI, `FreeCADCmd` and a server. The GUI closes a
depth-0 one on the next event-loop turn (`Document::setImplicitCloser`). They
signal open/commit to observers like any transaction (user, 2026-09-25). Each
row also carries the `MacroManager` lines issued meanwhile as a `script`
annotation; nothing replays it.

**Environment and sessions.** Each process opens a session row (`session`:
env, user, host, access, opened, closed, uuid); the environment
(`Application::Config()`: version, revision, OCCT, Python, Qt, system) is stored
once and named by the session. User and host names are recorded only under
`TransactionLogIdentity` (default off); the author of every row is always
recorded (section 11).

### 2.2 What a value is by who wrote it

The op's `derived` column holds who made the write:

| Value | Rule | Kept as |
|---|---|---|
| 0 by hand | anything else | durable |
| 1 derived | written while its own object `isRecomputing()`, decided at the first write in the transaction | per `TransactionLogDerived`: 0 none (op notes the change, no ref), 1 cache tier (default), 2 durable |
| 2 followed | written inside `App::TransactionFollowing` and the owner not recomputing: an element reference re-looked-up after the shape it names changed, or `Part::Feature` laying `Placement` onto `Shape` | durable, like a hand value |

- No per-property list: a Pad's `Shape` is derived, an imported body's or
  `obj.Shape = s` is an input. A filter by name would make an imported document
  unreproducible.
- Expression-driven properties are derived. An undo's op inherits the flag of
  the op it inverts. A later hand write in the same transaction clears it.
- Followed is not derived because a derived value may be dropped and nothing
  recomputes a reference on undo or branch return. Its point is the merge: a
  side whose only writes were followed has not changed the property (user,
  2026-10-09).

### 2.3 Touched state travels in the rows

A `set` op carries in `op.touched` the state BEFORE it: property Touched (1),
object Touch (2), Enforce (4), explicit request (8), taken at the property's
first write in the transaction (not for creates, writes during recompute, view
or document writes). A recompute record carries per object the state before
(`b`, `p`) and, if not clean, after (`a`, `q`). Undo, redo and restore rows also
write such a record, which is also where flag-only changes go. Every walk
(undo/redo, switch, restore, recovery) folds these (`TouchedFold`) and lands on
exactly the touched state the document had; an object whose derived value
could not be written back is touched last (user, 2026-09-28). The log's
"touched" is the property's own bit, not `PropertyLink::isTouched()`.

Why: the old rules (purge objects whose derived values came back; compare last
input with last recompute) gave wrong touched sets. Storing the state beside
the op that causes it was the user's idea (Journal 27.58).

### 2.4 What is not logged

- `Prop_Transient`, `Prop_NoPersist`, `Property::Transient`, and static
  properties transient by status.
- Bookkeeping writes: Save, Save As, branch rename and open writing the
  document's own `FileName`, `Label`, `TipName`, `LastModified*`, `History`,
  `Version`, `Branch` open no transaction and are forgotten by the log.
- A view provider's saved properties ARE logged with the log on, whatever
  `ViewObjectTransaction` says (user, 2026-09-24: "colours are important in
  CAD"); not logged: writes under an `App::WriteScope` of `ViewFit` (sizes a view
  computes). Other CAD tools classify by the kind of state, never by who made
  the change.
- The camera and other 3D-view state: to get a per-view, session-only stack
  (`docs/MultiViewEdit.md`), not built; `<Camera>` stays in `GuiDocument.xml`.
- Python proxy state (`obj.Proxy` attributes): never logged; those changes were
  never undoable (user, 2026-10-26). A save-side comparison of it is allowed
  (section 9.3).
- A write to an object under its own `Restore` status in a non-restoring
  document (a lazily served shape) is not a transaction.
- A transaction still open when its document is destroyed is committed but not
  logged.

A write that lands in no transaction (a `NoModify` property such as
`Visibility`, a `Legacy` one, a view's derived size, bookkeeping) makes the
log forget what it holds for that property, so the next snapshot serialises
it afresh (sec 39).

Known accepted leak: a write through a non-const reference that skips
`aboutToSetValue`. It is found by `TransactionLogVerify` and fixed at the site
(section 3.2).

Implementation: `TransactionStore.h` (interface, row structs),
`TransactionLog.{h,cpp}` (`number`, `onCommit`), `Transactions.{h,cpp}`
(`Transaction`, `TransactionObject::setProperty`, `PropData::shared`,
`inheritDerived`), `TransactionValue.{h,cpp}`, `DocumentObject::getLogTouchedBits`,
`Application::InvocationScope`. Python: `getTransactionLog()`,
`getTransactionOps(seq)`, `getTransactionValue(ref)`.

Journal: 9, 10, 11, 20.3, 21, 24.9-24.10, 27.9, 27.58-27.63, 30.8, 30.24,
31.22, 31.25-31.26, 38.22 (F1).

## 3. Values and storage

### 3.1 Values and entities

A value is what `Property::Save` writes: the XML fragment plus each attachment
it adds. No new serialiser exists, so every saveable property is loggable and
restorable by the existing `Restore`. All writers (a save's part, a captured
op value, a composed snapshot) use one byte format (archive configuration,
`fixed` float stream), or the log would hold three copies of a value.

Everything the log holds bytes for is an *entity*; entities are joined by one
kind of edge.

```
entity(hash PK, kind, enc, base, tier, size, data)
ref(entity, target, role, name, seq)   -- role: attach | base | part | blob | skeleton
```

- `hash` is SHA-1 of the full content (fragment plus ordered attachments). It
  never depends on how the row is stored, so re-encoding rewrites `enc`,
  `base`, `data` in place and nothing that names the entity moves.
- `kind`: `prop`, `attach`, `blob`, `xml`, `skeleton`, `composite`, `manifest`.
  `enc`: `raw`, `zstd`, `delta` (patch against `base`), `file` (a blob the
  document's `FileBlobManager` keeps; the log holds a handle).
- Every entity is inline in `entity.data`; a "large values as files" branch was
  never built.
- Blobs are entities. A value's referenced files are known exactly while it
  saves (`BlobRecorder`); a shape that borrows geometry from other files has a
  `blob` edge to each source. A blob's segment is durable before a row naming
  it commits.

**A version is a composite**, not a stored file: a *skeleton* (a container's XML
with each property body replaced by a placeholder) plus ordered (property,
part entity) pairs; one mechanism for `Document.xml`, each object's XML,
`GuiDocument.xml` and each view provider. Checkout composes the bytes and hands
`restore(dir)` an ordinary file, so "a version is a saved file" still holds and
rollback is a load. A save is *recorded* (every property serialised, the
version comes free); a snapshot is *composed* (a property whose newest value
the worker holds is claimed and not serialised; only misses run `Save`).

**Reverse deltas.** The newest entity of a chain is full; an older one is a
reverse delta (zstd `--patch-from`, level 3) against the one that superseded
it. A chain is re-anchored full at `TransactionLogDeltaHops` hops (8) or when a
patch exceeds `TransactionLogDeltaRatio` percent (50). Named versions are full
anchors. A value under 128 bytes stays raw (user, 2026-10-22).
Why reverse: evicting the oldest unnamed version, the common case, drops a
leaf; the newest, which recovery and embed want, is always full.
Why zstd: zstd level 3 matched zlib's ratio on BREP at 8-10 times the speed;
content-defined chunking failed because the solver perturbs every line.

**One collector.** What is held is computed once from op refs, manifests, kept
values and every edge role (`HeldGraph`, in memory); eviction, retention and
compaction all go through it (`dropTier`). Delta bases are followed from what
is kept to a fixpoint.

### 3.2 Immutable values and the shape freeze

A logged value is shared with the live document, so it must not change behind
the log. Three guards:
1. Compile time: list properties expose a non-const door only through a guard
   (`mutableValues(atomic_change&)`); `PropertyGeometryList` holds const
   geometry; non-const geometry transforms are explicit.
2. Run time, OCCT: the fork's `TopoDS_TShape::Immutable()` flag (distinct from
   upstream `Locked`, user, 2026-09-23) is set by `PropertyPartShape::setValue`
   and never cleared; builders throw on a frozen shape except for cache
   representations (triangulation, pcurves on new surfaces; user, 2026-09-24).
   Real changes copy-on-write (`Thawed` copies keep the original's name). A
   feature that raises `LockedShape` is recomputed once on thawed copies of its
   inputs (`Part::ThawInputs`). A frozen tolerance covers growth that is
   rounding only.
3. Audit: `TransactionLogVerify` (off; on under `FC_DEBUG`) serialises what a
   snapshot would have claimed and compares; `FC_TXNLOG_CHECK_COPIES` compares
   adopted copies with the live property.

`PartParams::ImmutableShapeValues` defaults on with the OCCT fork, off without.
`StableShapeBytes` (on) makes a saved shape's bytes independent of what was
later built from it. With the freeze off, `Property::canSaveOffThread()` is
false for a shape and it is captured on the main thread at commit.
Concurrency on frozen shapes: `BRep_RepresentationLock` (fork, striped mutex,
no ABI change) guards cache edits against the writer (user, 2026-10-01). A
non-main thread reads an element map only while it holds it
(`holdForOffThread`); names made meanwhile wait in a lock-free list on the map
(user, 2026-10-10: "keep the list"); the string table has a read/write lock.
Every gate runs with the freeze on and off.

### 3.3 The writer

One worker thread per file history. At commit the main thread decides which
copies are worth serialising (`isSame`, the derived rule), numbers the row,
copies every after value the commit would leave pending (streaming, user,
2026-09-25) and posts one job; the worker only ever serialises detached copies
(`shared_ptr<const Property>`), never a live property. Links and Python-object
values are captured on the main thread. The commit's after copy is kept in
`TransactionCopyCache` and adopted as the next edit's undo-before copy, so the
cost moved rather than grew. Reads go through a store that drains the queue
first. A shape's hash comes from its saved blob, so an edit-save cadence never
exports a shape twice. The newest 4096 rows are also kept in memory (`_recent`)
so a hot undo never waits for the worker.

Why: synchronous serialise-on-commit measured 60 ms for a sketch move and 2.85 s
for a snapshot (Journal 20); a writer thread was the prerequisite of an
always-on log.

### 3.4 The cache tier and its budget

Derived values live in an evictable tier. `TransactionLogCacheSize` (MB, 256)
and `TransactionLogCacheValues` (10000) bound it (0 = no limit), counting the
values plus bytes only they hold. Over either, `TransactionStore::evictTier`
removes values down to three quarters, per property oldest first, across
properties by age times bytes over the object's average recompute seconds
(cheap shapes go first). Kept regardless: the newest value of each property on
each branch, what a version lists, a kept value or object (section 7.6), and
any value a row by hand names -- the before of a hand write over a computed
value, which undoing that row writes back (sec 39). A
gone value reads as never kept: a cold undo skips it, touches the owner, and
the recompute remakes it. A delta whose base goes is rewritten full in the same
SQL transaction. The job runs when an eighth of a budget was added since the
last look. Python `evictTransactionCache(size, values)`.

### 3.5 The SQLite store

Choice (user, 2026-09-22): SQLite behind `App::TransactionStore`. A log
transaction is a SQL transaction, so crash atomicity is inherited; the
browser's questions are queries; WASM builds exist. Rejected: libgit2 (file
per value, gc, licence), LMDB, RocksDB, appending to the zip, a custom segment
file.

| Table | Holds |
|---|---|
| `meta` | key/value: identity, save id, copy id, pins, retention, counters (`seq_counter`, `last_object_id`), import maps, `compact_estimate` |
| `txn`, `op`, `name` | rows; ops (hashes as 20 bytes; type/property names as ids of `name`) |
| `environment`, `user`, `session` | who and where |
| `version`, `branch` | versions (manifest as an entity); branches (`from_version`, `from_seq`, `head_seq`, `last_id`, `closed`, `target`, `joined`) |
| `entity`, `ref` | values and their edges |
| `objname`, `objuid`, `lastgeoid` | file-scope allocators (section 3.6) |
| `strtable`, `strref` | the file's string table and who holds which ids |
| `heldfile`, `keepvalue`, `keepobject` | sent files held; values kept by hand |
| `access`, `mergerequest` | per-branch grants; merge requests |

- **Versioned layout.** `PRAGMA user_version` = `StoreVersion` (3 today: 1 the
  layout of 2026-10-19, 2 object UIDs, 3 the packed op table). A newer store is
  refused; an older one is migrated by `SQLiteStore::migrate(from)`, one step
  per layout change; a read-only older store is copied to memory and migrated
  there. Ruled "no schema, no migration" on 2026-09-28, REVERSED (user,
  2026-10-19) for the release aimed at the end of 2026.
- WAL with `synchronous=NORMAL`, `journal_size_limit` 1 MB,
  `wal_autocheckpoint` 256 pages; values are durable before the row naming
  them. Every batch writer goes through one SQL transaction with rollback.
  SQLite 3.38 or later is required. A read-only open writes nothing (no
  journal mode, no table, no migration); guard reads of an embedded copy open
  it immutable.
- Row scripts are stored tagged and compressed past 128 bytes; recompute
  records list only objects made up to date.
- A trim, squash or branch deletion vacuums the live store; a save does not.

### 3.6 One history per file: FileHistory and file-scope state

`App::FileHistory` is a process registry entry keyed by canonical path. It owns
what every open document of one file shares: the store, the worker, the row and
version counters, the branch table and its holders, the blob manager, the
transient directory and the file-scope allocators. `TransactionLog` is per
document and is only a cursor: its branch, head, pending values, undo stacks.
Registering a path another live history holds is refused. Paths are cleaned
and canonicalised, a not-yet-existing path through its existing parent.

Allocators are one per file (rulings of 2026-09-27), so branches and versions
of one file meet without collisions in a merge:

| State | Rule |
|---|---|
| Object ids | one counter per file, never lowered; on open the max of the saved `LastId`, ids restored, store meta and ops |
| Object names | one-to-one id <-> name; a name is never freed by delete; an object coming back under its id gets its exact name; a new object never takes a name the table gives another id (add `Pad001`, undo, add -> `Pad002`) |
| Object UIDs | one-to-one id <-> UID (section 10.6) |
| Geometry ids | per object, last id given (`nextGeoId`) |
| String table | the file's one `StringHasher` (section 3.7) |
| Labels | document scope; a merge resolves clashes |

`Document::compactFileState()` drops names and geometry ids nothing holds (no
open document, no op, no retained version); counters are never lowered. It runs
by itself after a trim, branch delete or squash once the estimated unreferenced
bytes reach `TransactionLogCompactSize` (KB, 256). A temporary document whose
history is its own restarts the ids on `clearDocument()`.

### 3.7 The string table

The file has one string table shared by all versions. A schema-5 save writes
it whole as archive member `StringTable.txt`; `Document.xml` only names it
(`<StringHasher2 table= hash= count= used=>`), so no version carries a table.
The log keeps it once (`strtable`), synced ahead of the jobs that need it. Each
version and each captured value records the ids it uses (`strref`, packed
ranges). A save compacts the table to what memory and retained versions and
values hold (user, 2026-09-29); ids are never renumbered or given twice;
`SaveAll` keeps everything. Strings of another document are imported by content
(section 10.2).

### 3.8 What travels into a saved file

Mode `TransactionLog`: 2 (default) a save writes the history into the file; 1
the history lives in the transient directory and dies with the session; 0 off,
a hidden A/B switch. The off mode as a user choice was withdrawn (user,
2026-09-22); the only user choice is whether a save carries history.

- **Archive.** `TransactionLog::embed` flushes, copies the store
  (`VACUUM INTO`), applies retention to the copy, drops its indexes, and hands
  it to the document's dynamic property `History` (`App::PropertyHistory`, a
  blob referrer holding the db and the blobs retained manifests name). On open
  the copy is adopted as the live store. A large `History.db` enters the
  archive raw-zstd. A save below schema 5 embeds nothing.
- **Retention of the copy** (the live store keeps every row). The copy carries
  each branch's newest version, the newest `snapshots` unnamed and `named` named
  versions per branch, every fork-base and pinned version; a bound that binds
  cuts the chain at the oldest version that travels, leaving a bridge
  (section 6.3). Numbers: store meta `keep_named`/`keep_snapshots` (they travel
  with the file), else `TransactionLogKeepNamed` (10; 0 every named; below 0
  the whole history) and `TransactionLogKeepVersions` (5). Python
  `getHistoryRetention()`, `setHistoryRetention(named=, snapshots=)`. Price: a
  fork worked on for more saves than the copy carries has no common row.
- **Forced history.** A file pinned by another file's link is saved with
  history whatever the preference (user, 2026-09-26).
- **Privacy.** History leaks what the author deleted. `saveCopy(path, False)`
  writes a copy without history (with a save id of its own, so the copy can be
  merged back by state).
- **The guard.** The embedded db cannot know the hash of the `Document.xml` it
  sits beside, so each save mints a `save_id`, written into the db `meta` and
  the dynamic `Version` property (`"<num> <uuid>"`), plus `LastModifiedDate`.
  On open both must agree; a foreign FreeCAD restamps the date. On mismatch the
  copy is adopted as closed branches: its `main` renamed `main@<save date>`, a
  new `main` starting from the file as found, version numbers continuing (user,
  2026-09-25). A save also records `meta save:<id>` with the version, the row
  and the trigger (`Std_Save`, `python:file:line`, ...;
  `getTransactionSave()`).
- **History from any file** (user, 2026-09-22): a file with no history gets
  version 1 = the file as found. An older-schema file is converted to schema 5
  at open with a warning naming Save As with the standard format as the way to
  keep an upstream copy (user, 2026-09-28; reverses keeping the file's format).

Implementation: `TransactionLog.cpp` (`TransactionLogCore`: `putEntity`,
`supersede`, `putComposite`, `composeEntry`, `embed`, `adoptStore`,
`syncStrings`), `TransactionStoreSQLite.cpp` (`Tables[]`, `migrate`,
`dropTier`, `evictTier`), `FileHistory.{h,cpp}`, `PropertyHistory.{h,cpp}`,
`StringHasher.{h,cpp}`, `FileBlobManager.{h,cpp}`, `Base/Writer` (entry
capture, `PropertySink`), OCCT fork `TopoDS_TShape`, `BRep_Builder`,
`BRep_RepresentationLock`.

Journal: 13, 14, 16.4, 16.6, 23.1-23.16, 25.4, 26.4, 27.7, 27.40-27.56,
27.61-27.65, 27.75, 27.81-27.86, 31.27-31.33, 37.4-37.6, 37.13.

## 4. Undo over the log

### 4.1 Model

Every undo, redo, selective undo and restore is a logged row. Undo of T is a
new row of kind `undo` with T's ops reversed, before and after swapped,
`inverts = T`; redo is the undo of the undo. Content addressing makes every
value a hit, so they store nothing new.

The undo and redo stacks hold rows. A step keeps its in-memory `Transaction`
while it is inside the *hot window* (`UndoMaxStackSize`, now a window size,
not a depth) and becomes a *cold stub* past it. Undo depth is the log's since
the document was opened. A new edit after an undo clears the redo stack, never
the log: the abandoned run stays browsable and restorable (it is not a branch).

**Hot path.** `Document::undo/redo` builds the inverse in the record it opens;
`logInverse` commits that record as kind `undo`/`redo`. The inverse inherits
each op's derived flag.

**Cold undo** (user, 2026-09-24). Past the window, the inverse ops are applied
onto the live document property by property; other objects, the view and the
selection are untouched. `_prepareRevert` checks everything before anything
moves (a removed object's id and name free and its type loadable, a created
object present, owners there, non-derived values present) and refuses the
whole step with reasons named; `_applyRevert` runs in passes (recreate removed
objects under their ids, reconcile dynamic properties, restore values, remove
created objects). A derived value gone from the cache is skipped and its owner
touched; never refused for that. Values go back under
`isPerformingTransaction()`, so object code that stands down for an undo
stands down here.

### 4.2 Undo steps and implicit steps

An *undo step* is the user's step plus the actor's implicit steps above it, up
to the actor's next user step (user, 2026-10-12: every change made without a
name rides with the step before it; revises the 2026-09-24 rule that a
recompute is a step of its own).
- `undo()` takes the run newest first, each from its copies if the document
  stands where the step left it, through the log otherwise; each writes its own
  `undo` row. All or none: a refused user step puts back the implicit ones.
- `redo()` takes the user step then the implicit ones under it.
- An implicit step ends no redo step. Implicit steps with no user step below
  are reached only from the panel. Names, counts and transaction ids are of
  user steps only.
- **Coalescing.** A hand commit of the one property the previous step wrote,
  same author and name, within `TransactionLogCoalesceSeconds` (3; 0 off) is
  folded into that step: one row, one undo step for a spin box or a Python
  loop. Versions, saves, switches, undos, other authors and object make/remove
  keep steps apart.
- `TransactionOnRecompute` is ignored with the log on.
- `setUndoMode` commits an open implicit transaction first, under the mode it
  was opened in.

### 4.3 Selective and multi-row undo

`Document::undoLogged(seqs)` (Python `undoTransactionFromLog(seq or [seq...])`,
panel "Undo row N" / "Undo N rows"). Allowed for a non-tip row on the current
branch when, for every property the row left in a state, the newest later op on
it left the same state (by hash, so undo-then-redo passes); created objects
still exist; removed objects' ids and names are free. Otherwise refused with
the conflicting ops named, nothing moved. Several rows are one step (user,
2026-10-12), taken newest first, each read as the newer ones left it; a later
row undone together is not a blocker, an unselected row between two selected
ones still is. The row is kind `undo`, named `Undo <name>` (or `Undo Pad,
Fillet`, `Undo N rows`), `inverts` the row or 0 for several. Derived properties
are neither checked nor restored (owners touched). It is a step of its own and
clears redo.

### 4.4 Refused steps

A step taken through the log is refused when a property it set was written
since to another value, an object it made is gone, one it removed cannot come
back under its id or name, or its row or value is gone. The refusal is said in
a dialog with a way past (user, 2026-10-12): **Skip this step** takes the next
step under it through the log (`undoSkipping`, Python `undo(skip)`); scripts
and served clients get `Refused` and `UndoRefusal`. A redo is refused only
where the same property was written since.

### 4.5 Per-author undo

Authors are recorded on every row (section 11). An author's stack is the steps
that are its user's (by kind and name, so it survives reconnects). Undo takes
the actor's newest step wherever it sits: from hot copies only when the
document stands where the step left it (a state token, not "last row"),
otherwise as a selective undo through the log, refused when another wrote the
same property since ("`<prop>` of id n was changed since, by row s"). A new
step ends only its author's redo steps. `undo(id)` down to another author's
step is refused.

### 4.6 Restore to a version

`Document::restoreVersion(num)` (Python `restoreTransactionVersion`) is a
forward transaction of kind `restore` named `Restore version N`, not a reload
(user, 2026-09-24): objects, pointers, selection and the undo stack survive,
and it is undoable. It walks the rows (`_moveAlongLog`): find where the two
chains meet, fold the rows back and forward per property, write only values
whose captured form differs, apply the touched fold. A move that changes
nothing records nothing. When the rows cannot make the state (chains do not
meet after a bridge-less trim, a value never reached the log, the path crosses
an open whose file changed outside its history) it falls back to a *whole
read*: the version opened as a hidden version document joined to the file's
history (blobs by hash, shapes shared), the difference applied, the document
closed. Document identity properties are never restored. Restore applies view
providers only under `ViewObjectTransaction`; a branch switch always does
(left inconsistent).
Why the rows: restore on a 148-object model took 3.9 s through a scratch
document and 0.08 s along the rows.

Implementation: `Document.cpp` (`undo`, `redo`, `logInverse`, `_takeRun`,
`_takeStep`, `_revertStep`, `_prepareRevert`, `_applyRevert`, `undoLogged`,
`undoSkipping`, `_coalesceStep`, `restoreVersion`, `_moveAlongLog`,
`_readVersion`, `_applyVersion`), `Transactions.cpp` (`Transaction::Cold`,
`LogSeq`, `StateBefore/After`, `absorb`), `TransactionLog.cpp` (`readRevert`,
`restoreValue`, `rowsBackTo`, `coalesceRows`), `TransactionStore::lastOpOn`,
`Gui::Document::undo(steps, ask)`.

Journal: 12, 24.1-24.13, 27.34, 27.57-27.60, 30.10, 33.4-33.5, 33.11-33.13,
37.4 (P4), 37.8.

## 5. Versions

### 5.1 Model

A version is an immutable point on a branch: number (per file, monotonic,
continuing across branch restarts), uuid (two unrelated "version 7"s differ),
kind, name, the row `seq` it stands for, environment, schema, and a manifest
(an entity listing the entries of the file the version stands for, including
`GuiDocument.xml`, so checkout restores view providers and camera). Shapes are
in the snapshot, so checking a version out is exact with no recompute; this is
the one place derived values are durable.

| Kind | Made by | Evictable |
|---|---|---|
| unnamed | every save; a snapshot every `TransactionLogSnapshotTransactions` (200) commits and on the `AutoSaveTimeout` clock (from when the log is made, so a never-saved document gets versions) | yes, oldest first, over `TransactionLogKeepVersions` (5) |
| named | the user; implicitly when a link pins it, a branch forks from it, a trim keeps it, or a merge's head is merged | never by the store |

- A snapshot (`snapshotToLog`, Python `snapshotTransactionLog`) is a save's
  serialisation into a null writer, composed from the log; refused inside an
  open transaction, during a restore, and on a partial document.
- Eviction never takes a named version or a branch's newest. Count only; a byte
  or cost limit was not built (cost weighting lives in the cache tier).
- A version a pin, a branch or a trim stands on cannot be un-named (sec 39);
  `nameTransactionVersion(n, "")` refuses it.
- A version holds what the document refers to, not its own `History`, so each
  file carries one history member.

### 5.2 Version documents

All open documents of one file share its history. Each is either a *branch
document* (holds a branch) or a *version document* (detached at a version).
Git's worktree rule (user, 2026-09-26): a branch is held by at most one open
document.

| Name | Meaning |
|---|---|
| `<file>@v<num>` | the FROZEN instance of a version (what pins show) |
| `<file>@<branch>@v<num>` | an EDITABLE instance at a version |
| `<file>@<branch>@` | a branch's tip (identity of a live link) |
| `<file>` | the file's own document |

- A version is open at most twice: once frozen, once editable (user,
  2026-09-26). A frozen instance refuses every data change, dynamic property
  change, branch operation, save, snapshot, keep and selective undo
  (`checkNotFrozen`); a recompute through links skips it. The panel greys its
  writing entries ("a pinned version: shown, not changed", user, 2026-10-22).
- An editable version document's first change makes it a branch document: it
  continues the version's branch if that branch is free and only records follow
  the version; else it makes the implicit branch `<branch>@v<num>`. A recompute
  of a detached document writes no record and makes no branch.
- Saving an editable version document offers Save to History (default), Save
  over File (last save wins; offers to re-pin links to the old version), or
  Cancel (user, 2026-09-26).
- A pinned version is read in part like a plain file (the objects links ask for
  and their dependencies); a later link wanting more re-reads the union.
- Opening: `doc.openTransactionVersion(num)`, `FreeCAD.openFileVersion(path,
  num)` (needs no document of the file; `FileHistory::openFile` reads a closed
  file's history from its archive and runs the guard).

Implementation: `TransactionLog.cpp` (`number`, `ensureBranch`,
`evictVersions`, `nameVersion`), `Document.cpp` (`snapshotToLog`,
`openFileVersion`, `openFileBranch`, `_restoreAsVersion`, `refreshVersionNames`,
`checkNotFrozen`), `FileHistory::parseName`.

Journal: 16.1-16.7, 25.3, 27.5-27.17, 27.22-27.29, 35.8, 37.5 (P6, P7), 37.8.

## 6. Branches

### 6.1 Model

Onshape's vocabulary: a branch (workspace) is a mutable named tip into the
`parent` tree; a version is an immutable point. `main` is id 1; every row and
version carries its branch; branch ids are never reused. `branch.target` is the
branch it was made from, the default side of its merges. Every walk goes
through `TransactionStore::chain(head, from)` (one branch) or `history(head)`
(also second parents). The file records the branch it is as the dynamic
`Branch` property beside `History` and `Version`.

### 6.2 Create and switch

- Branches are made only on purpose; undo is not branching.
- Created from a version (named `branch <name>` if it was not) or a row (a
  snapshot is taken); created at the current head, only a pointer moves.
  Creating switches to the new branch.
- A switch is in place: objects matched by id, pointers, selection and view
  kept, a `switch` record, no undo step. Each branch has its own undo stack;
  after a switch undo reaches back to the open only (user, 2026-09-25).
- A switch to a branch another open document holds is refused, naming it. A
  closed branch cannot be switched to, only branched from. A switch, and a
  branch made from an older point, are refused while an edit is open (a
  served client may be in it).
- `openTransactionBranch` (`Document::openNewBranch`) makes a branch at this
  document's head and opens it in a second document; this one stays. It names
  its fork version so eviction keeps it (sec 39). A branch is not its
  document: closing the document leaves the branch.
- `getTransactionBranchState()`: branch, target, rows with ops ahead and behind.
- A switch replays rows through the same walk as restore; while it replays,
  `isPerformingTransaction()` is true (user, 2026-09-29).

### 6.3 Trim, delete, squash

All three take rows out for good, are recorded (`trim`/`squash` rows), never
touch the current state of a branch, and are safe because every remaining op
carries both refs (user, 2026-09-22).
- `trimBranch(name, version, bridge=True)` removes the chain's rows up to the
  version except those another branch's chain holds; unnamed versions on them
  go except fork versions; the kept version is named (`trim <branch>`). With
  `bridge` (user, 2026-09-29), where another branch exists the stretch from the
  newest shared row up to the kept version is squashed into the kept version's
  own row, so a later switch still walks rows instead of a whole read; the undo
  floor rises to the kept version. The embedded copy's cut uses the same rule.
- `deleteBranch(name)` (not the current branch) removes its rows no other
  branch's *history* reaches: rows a merge stands on stay (user, 2026-10-06).
  It keeps, named, the versions a link pins and the versions at rows another
  branch holds (sec 39).
- `squashVersions(from, to)` rewrites the `to` row in place as one `squash`
  row parented on `from`'s row, its ops the net change (`netOps()`); seqs never
  change. Refused if a branch forks or a named version lies between, or the
  history between is not in the log. A squash/bridge row keeps the identity of
  the newest row it stands for and only the newest merge edge.
- All three refuse what another open document of the file stands on (a version
  document's version, a branch another document holds) (sec 39). Undo stacks
  are rebuilt from what is left. File-scope state is compacted afterwards
  (section 3.6). From a browser they are the host's alone (user, 2026-10-15).

### 6.4 Closed branches

When the guard finds the file edited outside its history, the embedded copy's
branches are adopted closed (section 3.8). A closed branch keeps its rows and
can be branched from or imported, never switched to.

Implementation: `Document.cpp` (`createBranch`, `switchBranch`,
`renameBranch`, `trimBranch`, `deleteBranch`, `squashVersions`,
`_checkoutHead`, `_arriveOnBranch`, `openNewBranch`, `branchState`),
`TransactionLog.cpp` (`setBranch`, `takeBranch`, `forkHere`, `adoptClosed`,
`planTrim`/`applyTrim`), `TransactionStoreSQLite.cpp` (`chain`, `history`,
`addBranch`, `replaceTransactions`). Signal `signalBranchesChanged`. Python:
`getTransactionBranches`, `createTransactionBranch(name, version=0, seq=0)`,
`switchTransactionBranch`, `renameTransactionBranch`,
`trimTransactionBranch(name, version=0, bridge=True)`,
`deleteTransactionBranch`, `squashTransactionVersions`, `openTransactionBranch`,
`getTransactionBranchState`, `getTransactionCursor`.

Journal: 17, 26.1-26.8, 27.42, 27.71, 29.6, 30.7, 31.13, 31.19, 37.5 (P1).

## 7. Merge and pick

### 7.1 The three-way merge

"Ours" is the branch the document is on, "theirs" the branch merged in (only
read). The **base** is the newest row in both histories (second parents
included); one base only. Each side's change is the net change base -> head
(`diffRows` folded by `NetChange`, the fold squash uses), compared by content
hash; no version is read. Two chains meeting nowhere are refused ("shares no
history") unless brought in as an independent branch (section 10.3).

| Theirs | Ours | Kind |
|---|---|---|
| changed | unchanged | `take` |
| changed | same value | `same` |
| changed | changed otherwise | `conflict` (`view` for a view provider) |
| recompute output | any | `derived` |
| created object | -- | `take create` |
| removed object | untouched, nothing of ours links to it | `take remove` |
| removed object | changed, or ours made/changed a linker | `conflict remove` |
| changed object | removed | `conflict revive` |

Rulings (user, 2026-10-04):
- Ours unchanged since the base: theirs is taken whole along the rows, derived
  values and touched state included, no recompute. Otherwise derived ops are
  skipped and the merge recomputes inside its step.
- A view-provider conflict is never asked: ours wins, listed on the row.
- An unpicked conflict refuses the merge with nothing moved; the caller picks
  or passes a fallback side. `MergeConflictSide` (0 ours, 1 theirs, 2 ask;
  user, 2026-10-12) sets the dialog's starting side only.
- Comparing branches in the 3D view: later (the shape diff, 7.5, is the
  answer built).
- Every value is read before anything is written; a value the log lacks
  refuses the merge. A label clash is suffixed and noted.

**The result** is one row of kind `merge` named `Merge <theirs>` with theirs'
head as second parent; an ordinary transaction (undo, replay, streaming). The
version at the merged head is named `merged into <ours>`. A merge that wrote
nothing of theirs is a record (no ops, second parent) so the base moves and a
kept-ours conflict is never asked twice. Theirs stays open and mergeable again.
A merge marks every object it changed for recompute and recomputes inside its
step (user, 2026-10-05); one undo takes back both. A merge that changes nothing
is no undo step (assistant's call, the user may overrule).

**Fast-forward.** A merge into a branch with no row with ops after the base
takes theirs' rows as they are, each under its own author, no merge row;
records of ours are re-appended after them. It only marks objects for
recompute, never computes (user, 2026-10-06). Not done when another branch or
cursor stands on a record to move. Undone row by row.

### 7.2 Merging inside a value

A merge may look inside a value, in three layers:

| Layer | Hook | Users |
|---|---|---|
| Element split | `Property::splitSaved` / `joinSaved`: things with a key and saved text | `PropertyLinkList` (by object), `PropertyExpressionEngine` (by bound path), `PropertySheet` (by cell) |
| Order policy | `DocumentObject::isMergedByElement(prop)` | `Group` yes; ordered lists (a loft's sections) stay whole |
| Unit | `getMergeUnit(prop)` + `mergeUnit(base, ours, theirs, ...)`: properties that are one thing | Sketch (`Geometry`, `Constraints`, `ExternalGeo`, `ExternalGeometry`); a look (`ElementAppearance`) |

Rules:
- Both sides changed one element differently: **the last writer wins** (newest
  row of each side since the base; tie or no time: ours); nothing is asked
  (user, 2026-10-05; for sketches 2026-10-06). The row records what the clock
  decided and for whom. Order: ours, then what theirs added; two sides adding
  the same thing add one.
- Sketch (user, 2026-10-06): geometry known by id; a constraint by key (type
  plus geometry ids/points); reference values ignored; a removal stands and
  constraints on removed geometry are dropped and reported. The merged sketch
  is solved in a solver of its own; if it does not solve cleanly the sketch is
  asked whole. Constraints an expression names by index get made names first
  (user, 2026-10-06, "auto naming"). A copy's geometry ids are rewritten into
  this file's at import.
- Face looks merge by element name only (user, 2026-10-06); paint differing on
  both sides follows `TransactionLogMergeFacePaint` (`ours` default, `theirs`,
  `asked`), not the clock.
- A unit taken whole carries what a recompute wrote with it.
- A side whose only writes were followed has not changed the property (user,
  2026-10-09). A reference taken from theirs is re-resolved after the values
  land: by name, else by geometry in the shape as it was, else marked missing.
- Whole-array properties otherwise conflict as wholes.

### 7.3 Pick, leave out, roll back

- **Pick rows** (user, 2026-10-06): `previewPick(rows)`, `pickRows(rows,
  picks, fallback)` apply chosen rows of another branch as one `pick` row; no
  branch is merged, no second parent. A row needing an object this document
  lacks is refused unless the creating row is picked; rows of this branch's own
  history are refused.
- **Leave out**: a change that asks nothing may be picked `ours` (a tick); such
  a merge is never a fast-forward and the row records `left`.
- **Roll back an undone merge** (user, 2026-10-07): a merge that writes takes
  the tip as an unnamed version first. The next merge on a head that is "a
  merge undone and nothing since" first rolls it back (branch to the row it was
  made on, merge and undo rows removed); with something done since it counts as
  merged. Also a command: `rollBackMerge()`. Not when another document stands
  on the rows, a branch was made from one or a named version sits at one.

### 7.4 Previews

`previewMerge(branch)` lists every change with its key, side and values.
`previewMergeResult(branch, picks, fallback, version, again)` (user,
2026-10-12): the tip read as a version into a log-less, view-less document,
the merge's write part applied there and recomputed; no row, no step, no
branch; it is kept and rewritten in place when the sides change. `again`
closes only a result it made (sec 39). The merge dialog is not modal (user,
2026-10-11): it goes stale and disables Merge when the preview no longer says
the same, belongs to its document, and is rejected only when its document
closes (user, 2026-10-22).

### 7.5 The shape diff

A diff is of one object's value at two states (live, branch head, version, row,
a preview's base/ours/theirs, the result). `Part::ShapeDiff` pairs the whole
shape first (one TShape or one rigid motion), then elements by name, by name
with one-sided steps taken out, by geometry, by neighbours; each entry says how
it was paired and what became of it (user, 2026-10-06). Names beat geometry
because references, colours and constraints hold faces by name. A material
diff (one general fuse) shows volume added and removed, on a thread above
`ShapeDiffInlineFaces` (300) faces. A shape the log let go is computed again
by opening the state at its row (`openTransactionState`). The overlay is drawn
by the view provider (`showValueDiff`), touches nothing, and is host-only when
the document is served (user, 2026-10-13); "Show last change" is an edit task
(`TaskShapeDiff`).

The plain naming step a feature writes on a passed-on element stays; the diff
sees through it (user, 2026-10-11).

### 7.6 Kept values

A save writes no cache tier, so a shape a recompute wrote is gone from the file
unless a version lists it. A value can be *kept* (user, 2026-10-11): one value
by hash (`keepvalue`, survives its row's trim) or every value of an object
(`keepobject`). Kept is the log's, not the document's: no row, no undo step,
marks the document modified only when something moved. A value is kept by the
host or by one who writes its branch; every value of an object by the host
alone; the keeper is recorded (user, 2026-10-14).

Implementation: `Document.cpp` (`previewMerge`, `mergeBranch`, `planMerge`,
`planWeigh`, `mergeByElement`, `_merge`, `previewPick`, `pickRows`,
`rollBackMerge`, `previewMergeResult`, `compareStates`, `openRowState`,
`keepLogValue`), `TransactionLog.cpp` (`mergeBaseOf`, `NetChange`,
`forwardBranch`, `reappend`), `Mod/Sketcher/App/SketchMerge.cpp`,
`App/ElementAppearanceMerge.cpp`, `Mod/Part/App/ShapeDiff.cpp`,
`Mod/Part/Gui/ShapeDiffView.cpp`, `TaskShapeDiff.cpp`. Python:
`previewTransactionMerge`, `mergeTransactionBranch(name, picks, fallback)`,
`previewTransactionPick`, `pickTransactions`, `previewTransactionMergeResult`,
`previewTransactionPickResult`, `rollBackTransactionMerge`,
`previewTransactionRollBack`, `compareTransactionStates`,
`getTransactionValueAt`, `openTransactionState`, keep/release methods,
`Part.getTransactionShape(doc, ref, tag)`, `Shape.diff`, `Shape.diffMaterial`.

Journal: 17.3, 28.1-28.7, 30.4 (P1), 30.7, 31.1-31.34, 32.1-32.20, 32.24,
32.28-32.35, 37.8.

## 8. Crash recovery

The log replaces autosave (user, 2026-09-25): `AutoSaver` writes nothing for a
log-on document.

- **Bounded loss.** Every commit streams its after values to the worker; a
  crash loses only the worker's queue and, on power loss, the last unsynced WAL
  transactions: always a consistent prefix.
- **What a crash leaves.** The transient directory with `history/log.db`, its
  WAL and blobs; the session row stays open. The Gui recovery dialog lists it
  (whatever the log preference), reading label and file name from the log's
  `meta`; it is "overage" when the file is newer than the log's last write.
- **Recovery** (`Application::recoverDocument`, Python
  `FreeCAD.recoverDocument`): take over the database and WAL (both or neither)
  and the blob store (sweep: per segment keep the newest generation that
  opens); anchor = the newest version, materialised and restored as an open
  would (none: start empty); fold the rows after it into an end state and apply
  it in a cold undo's passes forward, as no transaction. The replay stops before
  the first row whose non-derived after never reached the log; the head moves
  to the last applied row and the rest are removed and listed `skipped`. Touched
  state comes from the rows. Undo/redo stacks are rebuilt as cold stubs. A
  `recover` row records source, anchor, rows replayed and crashed sessions.
- **The recovered document keeps its history**: original name and label, log
  and blobs taken over; undo and the panel reach across the crash. It is left
  modified. It notes the file's state, so its first save pulls what another
  process saved meanwhile (sec 39).
- What is lost: the camera since the last version (bounded by the snapshot
  cadence).
- A folder document's recovery re-reads its in-place blobs from the folder; it
  still materialises the anchor's entries (a file per object). Recovering from
  the document's own folder, or from the store without materialising, was not
  built.

Implementation: `TransactionLog.cpp` (`onCommit`, `recover`, `noteIdentity`,
`readRecoveryMeta`), `Document.cpp` (`recoverFromLog`, `_replayLog`,
`_rebuildUndoFromLog`), `FileBlobManager::recoverStore`,
`Gui/DocumentRecovery.cpp`. Check `scripts/transaction-log-recovery-check.py`.

Journal: 25.1-25.8, 37.1, 37.8, 38.17.

## 9. Files and saves

### 9.1 Saves into an archive

- Every save records an unnamed version (`save` row: version, `Document.xml`
  hash, blobs, path) and, at mode 2, embeds the history (section 3.8). Save As
  continues the same history; only the path differs.
- **saveToLog** (Python `doc.saveToLog()`, `Std_SaveToHistory`; user,
  2026-09-26): records a named version of the branch without rewriting the
  file's document members. The archive is rewritten with members raw-copied and
  a new history member that keeps the guard matching; the file still opens as
  before and the document stays modified. Needs history in the file. The
  rewrite is renamed over the file in one step (sec 39).
- **saveVersionAsFile** writes the file from a version document; the file
  reopens on the saving document's branch.
- **saveCopy(path, withHistory)** writes a copy; without history it carries a
  save id of its own so it can be merged back by state.
- A save or a copy onto the file of another open document of this process is
  refused (sec 39): that document reads the target's blobs in place
  (section 9.3).
- A save below schema 5 writes schema 4 with no log capture and records a
  schema-5 snapshot of it.

### 9.2 Two processes on one file

Choice (user, 2026-10-24): the file is the meeting point, with a pull in every
save. A live store shared through SQLite between processes (reserved id blocks,
polling) was not built; it would be considered only if live co-editing between
processes is asked for that a shared session cannot give. A save with no second
writer must not be slower.

- **Detecting.** `App::DiskStamp` (size, mtime ns, inode) is taken when a
  restore starts and after each save. At save: same -> proceed; different ->
  read the file's `Version`: our save id means only touched; otherwise another
  process saved (`isFileChangedElsewhere()`). A history-less file's `Version`
  does not count.
- **Save lock.** `.~<name>.lock` beside the file (`App::SaveLock`, boost
  `file_lock`, released by the OS at process end, never stale), held from the
  look to the end of the write; waits up to 60 s then fails naming who holds
  it. Taken on every save, archive or folder, history or not; a history-only
  save locks before it reads.
- **Open marker.** `.~<name>.open`, one line per opening process (host, pid,
  opened, last seen); warns that the file is also open elsewhere; blocks
  nothing; lines of gone processes of this host are dropped; a process's line
  is seen again at each save, and a line of another computer not seen for
  `FileOpenerExpiryDays` (30) is taken as gone, with a warning (sec 39); the
  last closer removes marker and lock (`getFileOpeners()`).
- **Pull** (`_pullFromDisk`, under the lock, at save only -- an automatic pull
  would rewrite the document under the user): copy the file to
  `<TransientDir>/pull/<n>/`, import it as a fork branch `<file> saved
  elsewhere` (authors `host (<file> saved elsewhere)`), merge. Clean: the save
  writes. Conflict: the save stops with nothing written, the branch stays
  pending and the panel opens the picker; the next save re-merges. After the
  write the branch is closed; each later pull is a fresh branch. A pending pull
  remembers its file; a save elsewhere, or with that file gone, lets it go
  with a warning (the branch stays to merge by hand).
- A file without history merges by state: the base is the row this document's
  log was at when it last read or wrote the file, and the file comes as one
  row against it.
- The panel stats each open document's file every 3 s and says once that it
  was saved by another process.
- `TransactionLogPullOnSave` (on): off, a save writes over the file (the lock
  is still taken).

### 9.3 A document saved as a folder

The format (this fork's, since 2019): `Document.xml` (the commit point), one
`<Name>.xml` per object (and per view provider), `blobs/` with readable names
and an index `blobs/Content.xml`, `History.db` beside `Document.xml`, and the
string table. Plain uncompressed files. User, 2026-10-24: "with transaction
log, deal with directory is better than archive file, if we can get things
right". Why: an archive is rewritten whole every save, so its history is
recopied each time; a folder's files are independent, so a save can cost what
changed, history can be updated rather than recopied, blobs read in place, and
diff/backup/VCS tools see per-object files.

**Atomic save (stage, record, move).**
1. Every entry is written into a stage `.~<name>.save` beside the folder;
   staged files whose bytes the folder already has are dropped from the stage.
2. The record `.~commit` (written through a rename) lists what to delete.
   Writing the record is the commit.
3. The rest is renamed in, `Document.xml` last; deletions run; the stage goes.
4. With `FolderSaveSync` (on; user, 2026-10-26) staged files and directories
   are flushed before the record, so power loss leaves the old save or a
   recorded stage. Off for slow network drives.

A save cut short is settled by whatever next finds the stage (open, save,
save-as): with a record it is moved in, without one deleted. A record line that
is absolute, leaves the folder or holds `\` or `:` voids the record. Forty
random kills opened whole every time. An open takes no lock; it stamps
`Document.xml` before and after and reports a save that happened meanwhile.
A read-only folder opens; its edits go to the transient store; a save fails at
the stage. No backup file is made (user, 2026-10-26). An archive extracted by
hand opens as a folder; a zipped folder is not read.

**History.db brought up to date.** The live store records changes with
SQLite's session extension from the open or the last save. A save copies the
folder's `History.db` into the stage, checks it is the save the record started
from (`save_id` and the per-copy `copy_id`), plays the changes in, applies
retention, and S1's commit moves it with the rest, so history and
`Document.xml` go in together. It is copied whole instead when the record
cannot be trusted (first save, store reopened, the copy is another save, a
change does not apply, the extension is missing, or the copy fails the check
that every version and kept value that should travel is there). A history-only
save of a folder changes only `History.db`, the History element and `blobs/`.
Cost is rows since the last save, not the whole history. A live store kept in
the folder is a later opt-in for local disks only, never default (user,
2026-10-25).

**Blobs read in place** (user, 2026-10-25). A document opened from its own
folder reads `blobs/` where it lies; the transient directory no longer copies
them and holds only commits since the save, blobs made since, scratch and what
a crash leaves. Guarantee that a name never silently changes meaning under a
reader: before a save replaces or prunes a file read in place, this process
copies it into its store; while another process has the folder open (a marker
line not ours), a save gives no name new content and prunes nothing. Save-as
re-points to the target's same-content files. Folders that go away (checked-out
version, recovery anchor, pull copy) are copied as before. Known weakness: a
folder moved or deleted by hand while open loses its blobs.

**Keeping unchanged object files.** After a save (or an open) of its own folder
the document remembers the folder stamp, the options that decide bytes, the
version recorded, and per object its entries, blobs and proxy state. An
object's file is kept (not serialised, not rewritten) when all hold: same
folder, options and stamp; the last save's version still in the log; no later
row on this branch names it; no change noted outside the log
(`noteSavedChange`: status changes, `NoModify` writes, restore afterwards);
same blobs; same name; same element-map ids; same Python proxy state. Its
`beforeSave` still runs. `FC_FOLDER_SAVE_CHECK` writes everything and compares;
the gates run with it (user, 2026-10-25). A save numbers element maps so a map
keeps its last-saved number (user, 2026-10-26, "make element map stable"), so an
edit writes one file.

**BlobCompression.** A document Format property `Zstd` (default, preference
`BlobCompression` 0) or `Deflate` (level 1, every archive entry extractable by
any zip tool) (user, 2026-10-25). A folder writes plain files either way.

Further work on save cost stopped at a 600-object save of 75-124 ms (user,
2026-10-26: "stop here"); floating retention and the other ideas are listed in
section 14.

Implementation: `Document.cpp` (`saveToFile`, `restore`, `saveToLog`,
`_saveFolderHistory`, `_pullFromDisk`, `_takeFolderRead`, `noteSavedChange`),
`FileElsewhere.{h,cpp}` (`DiskStamp`, `SaveLock`, open marker,
`App::FolderSave`: `freshStage`, `commit`, `rollForward`),
`TransactionLog.cpp` (`embedFolder`, `travelling`), `TransactionStore`
(`beginChanges`/`takeChanges`/`applyChanges`), `FileBlobManager`
(`readInPlace`, `planSave`, `prune`, `writeIndex`, `rewriteArchive`),
`Base::FileWriter`. Python: `isFileChangedElsewhere`, `getFileOpeners`,
`getLastSavePull`, `getLastSaveHistory`, `getLastFolderSave`,
`getTransactionSave`.

Journal: 13.3, 27.16-27.18, 27.28-27.29, 29.7-29.8, 38.1-38.22.

## 10. Across files

### 10.1 Pinned links

External links pin versions, never branches (Onshape's rule, made optional).
`PropertyXLink` saves `version="N" vuuid="..."` when pinned (an old FreeCAD
then resolves the live file) and `branch="..."` for a live link to a branch;
both are part of the value, so changing a pin is an undoable `set`.

- A pinned link resolves to the frozen instance `<file>@v<num>`, opened from
  the linked file's history (a loaded log, or the archive's embedded history,
  with no document of that file needed). On any failure (no history, no
  version, uuid mismatch, missing blob) it warns once, resolves to the live file
  as an unpinned link, keeps the pin and retries on the next open (user,
  2026-09-26).
- A link to an editable instance is live, never pinned automatically; pinning
  is the user's act; pinning a link to a changed editable document first saves
  it to the log and pins that version (user, 2026-09-26).
- `pinLink(link, version=0)` names the version `pinned` and records the pin in
  the linked store's meta `pins` (so eviction, branch delete and the copy's
  retention keep it); the linked file is then saved with history. Python
  `obj.pinLink(prop, version=0)`, `unpinLink`, `getLinkPin`; Gui `Std_LinkPin`,
  `Std_LinkUnpin`.
- A link may pin a version of its own file (XLink only; saved with an empty
  file so Save As keeps its meaning; user, 2026-09-26).
- Pins are not transitive (user, 2026-09-22): a live link's stamp differs on
  open, the assembly is marked for recompute.
- A pinned link never takes part in a cross-document transaction. Each document
  of a cross-document transaction logs itself; the shared id is recorded for
  correlation.
- When the last link to a frozen version opened for a pin goes:
  `ClosePinnedVersion` 0 ask (default), 1 close, 2 keep; with no Gui, ask keeps
  (user, 2026-09-27).

### 10.2 Shapes and strings shared across documents

All documents of a file share one blob manager, so parsed shapes and
triangulations are shared; with frozen shapes the parse cache is keyed by
content hash, so two files with the same bytes share one parse. A shape moved
rigidly on restore is a shallow move (new top, shared children).

Element names of another document's shape are imported into this file's string
table by content (`StringHasher::importID`, `Data::translateElementMap`): every
`#id` in a shape's names is an id of the shape's own table. A cross-document
marker names its document (`;:X#<id>`). A reference into another document
saves its element name with ids in the owner's table (`stored=`) and finds it
by content in the target's on load, else by geometry, else marked missing. A
reference holds the string ids it names (`sids=`), including expression paths
and sheet cells.

### 10.3 Forks: another copy's history brought in

Rulings R3/R4 (user, 2026-10-04): branch and merge are explicit; a copy of the
file worked on apart is a *fork* and comes back as a branch to merge. Copies
are clones: the same history meeting again is two branches of one history
(user, 2026-09-22).

- **Row identity.** `seq` and version numbers are per store, so a row is known
  by `(session.uuid, ord)` (`uid` in Python rows). `forkBase` walks the other
  chain for the first row this store holds.
- **Import** (`importFork(path, branch, sender)`, Python
  `importTransactionFork`; `getTransactionForkBranches` lists what to choose
  from). Rulings F1-F8 (user, 2026-10-04): the copy's history is opened
  without its document; a branch is made at the base; each copy row with ops
  is replayed, oldest first, as one transaction (objects under new ids if
  theirs collide, names kept unless taken, values via `restoreValue`, derived
  values left out and owners touched); the imported row keeps the copy's
  identity and sessions; named versions come renumbered, unnamed do not; the
  user picks one branch of the copy; the branch is named after the file; a
  second import continues it; an unappliable row stops the import, rows before
  kept, and a re-import resumes. An `import` record ends it. Then the merge is
  section 7, explicit.
- **Maps.** Ids, names, strings, element tags and sketch geometry ids are
  rewritten into this file's (`App::RestoreNames`, `RestoreStrings`,
  `RestoreMinted`), maps kept in meta `import:<branch>`. Objects are the same
  when made by a row both stores hold, else by UID and type (section 10.6); a
  merge of this file's rows replayed in the copy stays a merge only when it
  holds everything its second parent said.
- **A file without history** (G1-G8, user, 2026-10-05): the base is the save
  its `Version` names; the file comes as one row `As found: <file>` carrying
  every differing value; same file again with an unchanged `Document.xml`
  brings nothing.
- **Independent branch** (user, 2026-10-05): a file that shares no history is
  still mergeable. Its rows form a branch from row 0; the merge has no base:
  same content = `same`, other = `conflict`, what only theirs has comes, what
  only ours has stays; never a fast-forward.

### 10.4 Requests and sent files

Rulings H1-H7 (user, 2026-10-05). A *request* is derived, never stored: a
branch with an import mark whose head this branch's history does not hold
(`getTransactionRequests`). A client of a served document that may write can
send its copy (`requests.send`); the host alone chooses where it lands, and a
file sent is **never read on arrival**, only when the owner brings it in from
the panel. Arrival is a `request` row under the sender's session; the bytes
are one blob held by `heldfile` keyed on the row identity, travelling in the
file (not in a copy without history), let go when the branch it came to is
merged, deleted, trimmed or dropped (a `drop` record). No age limit; the sender
is not told. Limits `UploadLimitMB` (16; up to 1024 with pieces) and
`RequestsTotalMB` (64). The sent-file Python entry points that read or write a
host path are gated as `saveAs` is (`fs.read` / `fs.write`) (sec 39).

### 10.5 Linked sets

An assembly is a set of files linked by XLinks. Branches stay per document; a
set is a record, not a store concept (user, 2026-10-17: "develop import for
assemblies"; Q1-Q9 as proposed, 2026-10-18).

- **Export** ("pack and go"; `exportLinkedSet(folder, withHistory=False,
  overwrite=False)`, `Std_ExportLinkedSet`): the closure of the top's links;
  files inside the top's folder at their relative path, files outside under
  `_external/`; a pinned version or branch becomes its own file
  (`part.v3.FCStd`, `part.side.FCStd`); links rewritten plain and relative so
  any FreeCAD opens it; copies without history by default, each with its own
  save id; manifest `FreeCAD-set.json`.
- **Plan and pairing** (`planLinkedSetImport(path)`): their set is read from
  each `Document.xml` without opening it. Pairing, first that answers: a shared
  history, the save id a no-history copy names, our manifest, the document
  `Uid`, shared object UIDs, the same path under the top, none. Two of ours
  answering alike: ambiguous, the user says. The first three are certain
  (default Merge), the rest guessed (default Take theirs, because a stranger's
  file merged by id and name puts every object twice).
- **Import** (`importLinkedSet(path, pairs)`): files imported linked-first as
  fork branches; their relative links mapped to ours; links rewritten to the
  branches they came to ("Link their set" rows). A file of theirs alone is
  held in the top's log (kind `set`) and written to
  `<TransientDir>/held/<set id>/<path>`, never into our folder at import; a
  held path read from a file that leaves `held/` is refused (sec 39). A file
  of theirs outside their top's folder is written under `_external/` in ours,
  and a write onto a file of their set is refused (sec 39).
- **Merge** (`mergeLinkedSet(files, overwrite, name, set)`): actions per file
  `merge`, `theirs`, `ours`, `write`, `drop`; nothing moves unless all can;
  dependencies first; **one undo step across the documents** (user,
  2026-10-18, Q3). Take theirs = ours restored to their branch head (one row,
  ours kept in history) plus a record with their head as second parent; Keep
  ours = that record alone. Undoing the set merge leaves every file undecided,
  Keep ours too (user, 2026-10-27): the set is pending again, and its re-merge
  first takes each Keep-ours record of the undone step back out of the log; a
  file changed since keeps it, and a Merge of that file is refused with the
  reason (sec 39).
  `mergeLinkedSet(write_to)` is gated `fs.write` (sec 39).
- **Preview** (`previewLinkedSet(files)`; user, 2026-10-23): each file's result
  is built and every reference to another file of the set resolved; failures
  are listed and the dialog offers to merge the referred file too.
- **Records.** Each file's `import` record names the set; the top's log holds
  a `set` record (pairing, actions, held rows) and a `set` record `merged`
  ends it. `getLinkedSetImports()`, `Std_ImportLinkedSet`.

### 10.6 Object UIDs

Every object has a random UUID (`obj.ObjectUid`, read only), not a property
(no undo, never edited), saved as `uid` on its `<Object>` entry; the history
keeps one UID per object id (`objuid`). Rulings (user, 2026-10-21):
1. A new UID only when an object is created, or brought into a document where a
   live object already has that UID (a copy into its own document: paste,
   Duplicate, Link copy-on-change).
2. Objects pair in a merge or import by UID and type; the name is ignored.
3. A UID is a history link: an object pasted back after deletion is the same
   object and returns under its old id and name. One UID, one id.
4. Name reservation stays; Take theirs hands a name to theirs when ours that
   held it goes in the same row.

Precedence when an object takes its id: the history's UID for the id; the UID
it carries; for an id the file knew, a v5 UUID derived from the file's `Uid`,
id and name (older files, so two copies agree); else its own. Lazily made
origin features derive theirs from the origin's UID and role. A save below
schema 5 and every export also write the UID as property `_ObjectUUID` so
upstream round-trips it. Why: identity by id and name forced names to be
reserved forever and made Take theirs rename every object.

### 10.7 Paste clashes and owned objects

An object brought (paste, merge project, copy-drag) whose UID and type match a
live one is a *clash*. Three takes: **Duplicate** (new object, new UID; the
default), **Assign** (the live object keeps id and name and takes the state
brought; what was brought refers to it), **Exclude**. The question is asked
before the step opens (user, 2026-10-22): the stream's UIDs are scanned, a
clash imports into a scratch document for the dialog, then the real import
runs with the answers; Cancel imports nothing. Rules to a fixpoint: an
included object an assigned one depends on and that is here too is assigned;
an owned object follows its owner. "Assigned objects take the look brought too"
(default on).

An *owned object* is made with its parent, removed with it, never another's
(`getOwnedObjects()`, Python proxy hooks): origins and their datums, a link's
copy-on-change group and array elements, a TechDraw page's views, Assembly's
joint groups. An owner pairs a slot by place; content pairs by UID or not at
all and follows the owner. A drag out of an Auto Delete link group is a move
(user, 2026-10-22).

Implementation: `PropertyLinks.cpp` (`PropertyXLink` pins, `ImportSetLinks`,
`ShadowSub`), `Document.cpp` (`pinLink`, `importFork`, `forkBranches`,
`_importReplay`, `_applyForeignState`, `importRequests`, `keepSentFile`,
`_giveObjectUid`, `sameObjects`), `TransactionLog.cpp` (`forkBase`, `rowId`,
`findRow`), `FileHistory::findVersion/openFile`, `ExportSet.cpp`,
`ImportSet.cpp` (`planSetImport`, `importSet`, `mergeSet`,
`previewSetReferences`), `Gui/DlgImportClash.cpp`, `DlgExportLinkedSet.cpp`,
`DlgImportLinkedSet.cpp`, `Gui/SceneRequests.cpp`.

Journal: 16.5, 17.4, 18, 27.1-27.39, 27.74-27.82, 30.12-30.23, 30.28-30.29,
30.33-30.38, 31.14-31.15, 35.1-35.18, 36.1-36.7, 37.9-37.12.

## 11. Users, access and serving

**Shared head** (user, 2026-10-04): clients of a shared session write to one
shared head -- one document, one line of rows, a row per operation (R1); the
author of every row is recorded (R2); branch and merge are explicit (R3); a
clash on the shared head is last commit wins (R5). An earlier design (a branch
per writer, merged after each operation) was built and withdrawn the same day:
every idle document moved along the log on the main thread.

**Users and sessions** (U1-U6, user, 2026-10-04). Table `user` kinds: `local`
(the desktop; the OS name only under `TransactionLogIdentity`, else `host`),
`verified` (identity from a signing front door), `invited` (an invitation the
host issued to one named person), `declared` (a typed name: login rows only,
never an author), `enrolled` (a browser counted by a grant), `fork` (authors of
an imported copy), `group`. A session is one process or one login; a login is
a `login` record (hidden in the panel by default). Only a verified, invited or
enrolled user writes; others are view-only whatever the grant says; the
desktop writes as before. `App::Actor` and `ActorScope` set the thread's actor;
a transaction's author is fixed at open.

**The door.** An identity header is believed only when the server says its
proxy signs people in (`FC_SERVE_IDENTITY_DOOR`); a token is half of a user and
a browser-generated device id the other half: a grant may count browsers
(`maxUsers`), enrolling the first N and refusing the rest (user, 2026-10-05).
Under a counting grant every plain request must name its device.

**Branch access** (user, 2026-10-14, amended 2026-10-15). The host and the
desktop may do everything. Anyone else has per branch *read* or *write from a
point* (a version of that branch, default the fork), stored in table `access`;
groups nest and lend their access (`addTransactionGroup`,
`setTransactionUserGroup`). Read: see the branch, apply its rows, request
merges; in the working branch edit, undo own rows, snapshot. Write: also
restore, switch, branch from, name versions, handle merge requests into it;
nothing before the point. A branch a client makes is its own; in a document
served from a branch it is written by every joiner and deleted only by its
maker or the host. Unreadable branches are listed greyed by name. Delete,
trim and squash are the host's ("takes rows out for good"), except a client
deleting its own branch. Seeing the log needs the right to edit (user,
2026-10-13).

**Merge requests.** Table `mergerequest` (source, target, who, note, state
open/merged/rejected/withdrawn); a merge answers open requests however it was
done.

**Serving a branch.** `Gui.serveBranch(doc, name, port)` opens the branch in a
window-less second document and serves it; the sharing panel lists served
branches.

**The browser History panel.** The browser has its own History card
(`web/src/logpanel.tsx`) asking the host through ops in `Gui/SceneLog.cpp`, all
run as the asking user: read (`log.rows`, `log.ops`, `log.value`,
`log.versions`, `log.branches`, `log.kept`, `log.requests`, `log.preview`) and
act (`log.undo`, `log.snapshot`, `log.nameVersion`, `log.restore`,
`log.branch`, `log.switch`, `log.apply`, `log.request`, `log.mergeRequest`,
`log.rollBack`, `log.keep`, `log.trim`, `log.squash`, ...). A `log.changed` cue
tells every reader to ask again. Mirroring the desktop dock was rejected
(shared selection, context menus). No op opens a document from a page.

**Uploads in pieces.** 4 MB pieces appended to a part file as they come, size
checked first, one live upload per connection (four in all), a resume token,
ceiling 1 GB. The pieces live in a per-process folder (sec 39).

Implementation: `App/Actor.{h,cpp}`, `Document.cpp` (`setBranchAccess`,
`userGroups`, `requestMerge`, `closeMergeRequest`), `Gui/SceneActors.cpp`,
`Gui/SceneLog.cpp`, `Gui/SceneRequests.cpp`, `Gui/Renderer/SceneServer.cpp`
(`writerRule`, devices, grants), `Gui/Renderer/web/src/logpanel.tsx`,
`logmodel.ts`. Python: `getTransactionSessions`, `pushTransactionActor`,
`popTransactionActor`, `transactionLogin/Logout`, `setTransactionAccess`,
`clearTransactionAccess`, `getTransactionAccess`, `getTransactionUsers`,
`requestTransactionMerge`, `getTransactionMergeRequests`,
`closeTransactionMergeRequest`, `Gui.serveBranch`, `Gui.serveGrants`,
`Gui.serveDevices`. Settings: `Preferences/SceneShare` `UploadLimitMB`,
`RequestsTotalMB`, `Devices`. See also `docs/ShareAccess.md`.

Journal: 29.1-29.6 (withdrawn), 30.1-30.12, 30.20-30.32, 30.40, 32.21-32.37.

## 12. The Gui panel

`Gui::DockWnd::TransactionLogView` (`Std_TransactionLogView`) began as the log
browser, built alongside the log as both deliverable and verifier (user,
2026-09-22), and grew into its manager. It shows: a filter and Snapshot; the
branch bar (switch, Branch..., Rename, Delete, Merge..., Merge from file...,
Branch to document, All branches, Hide records, Logins); a status line
(embedded or session, branch, ahead/behind, another process saved); three tabs
-- Transactions (newest first, with a `git log --graph`-style lane pane, user,
2026-09-25: "display branching like git UI"), Versions (with "Changed since"
and "Entries"), Kept -- plus a Requests list; the detail of a row's ops; a
value pane; and the diff bar (pair of states, slider, "Objects:" list chosen
like an attachment's support, user, 2026-10-11). The merge dialog lists
conflicts first with base/ours/theirs, a per-change pick and tick, a preview of
the result in the 3D view, and goes stale rather than lying. A pinned version's
writing entries are greyed. The user's own view is in
`docs/TransactionLogGuide.md`.

Implementation: `Gui/TransactionLogView.{h,cpp}` (`TransactionDiffBar`,
`MergeDialog`, `askBeside`, `mergeSaved`, `checkDisks`). Checks:
`scripts/transaction-log-*-check.py`.

Journal: 21 (panel), 22.2, 26.7, 28.6, 31.12-31.13, 32.9-32.20, 33.11-33.13,
37.8.

## 13. Implementation map

### 13.1 Files and classes

| File | Carries |
|---|---|
| `src/App/TransactionStore.h` | the store interface and row structs (`LogTransaction`, `LogOp`, `LogVersion`, `LogBranch`, `LogRowId`) |
| `src/App/TransactionStoreSQLite.cpp` | the SQLite store: `Tables[]`, `StoreVersion`, `migrate`, `chain`, `history`, `lastOpOn`, the collector (`HeldGraph`, `dropTier`, `evictTier`), delta codec, change sets |
| `src/App/TransactionLog.{h,cpp}` | `TransactionLogCore` (one per file: worker, counters, entities, embed, recover, strings, allocators) and `TransactionLog` (one per document: the cursor, `onCommit`, `number`, branch, pending values, `_recent`) |
| `src/App/TransactionValue.{h,cpp}` | `captureValue`, `restoreValue`, hashing, `CaptureConfig` |
| `src/App/Transactions.{h,cpp}` | the in-memory `Transaction` (hot window, cold stubs, `LogSeq`, author, state tokens), `TransactionCopyCache`, `TransactionFollowing` |
| `src/App/FileHistory.{h,cpp}` | the per-file registry, `openFile`, `findVersion`, `parseName`, `canonicalPath`, fork reading |
| `src/App/FileElsewhere.{h,cpp}` | `DiskStamp`, `SaveLock`, the open marker, `FolderSave` (stage, record, roll forward) |
| `src/App/PropertyHistory.{h,cpp}` | the `History` property: the embedded store and its blobs |
| `src/App/Document.cpp` | everything the document does with the log: undo/redo runs, cold and selective undo, restore, switch, branches, trim/squash, merge, pick, previews, recovery replay, pull, folder save, UIDs, pins, imports, access |
| `src/App/Actor.{h,cpp}` | actors and their scopes |
| `src/App/ImportSet.cpp`, `ExportSet.cpp` | linked-set plan, import, merge, preview; pack-and-go export |
| `src/App/ElementAppearanceMerge.cpp` | merge of element looks by name |
| `src/App/StringHasher.{h,cpp}`, `ShadowSub.h` | the file's string table; references holding their string ids |
| `src/App/FileBlobManager.{h,cpp}` | blobs, pack store, in-place folder blobs, recovery sweep |
| `src/Base/Writer.*`, `src/App/PropertyContainer.cpp` | entry capture and the property sink (record vs compose) |
| `src/Mod/Part/App/PropertyTopoShape.cpp`, `ShapeDiff.{h,cpp}` | shape freeze, off-thread save, parse cache; the shape diff |
| `src/Mod/Part/Gui/ShapeDiffView.cpp`, `TaskShapeDiff.cpp` | the diff overlay and edit task |
| `src/Mod/Sketcher/App/SketchMerge.cpp` | merge of a sketch as a unit |
| `src/Gui/TransactionLogView.{h,cpp}` | the panel, diff bar, merge dialog |
| `src/Gui/DocumentRecovery.cpp` | the recovery dialog's log candidates |
| `src/Gui/SceneLog.cpp`, `SceneRequests.cpp`, `SceneActors.cpp` | browser History ops, sent files and uploads, actors of a served session |
| `src/Gui/DlgImportClash.cpp`, `DlgExportLinkedSet.cpp`, `DlgImportLinkedSet.cpp` | paste clash, set export and import dialogs |
| `src/Gui/Renderer/web/src/logpanel.tsx`, `logmodel.ts` | the browser History card |
| OCCT fork (`~/works/sw/occt`, `LinkVibe-801`) | `TopoDS_TShape` Immutable/Thawed, `BRep_Builder` checks, `BRep_RepresentationLock`, stable shape bytes, pcurve-omittable cache |

### 13.2 Settings

`DocumentParams` (`Preferences/Document`) unless said otherwise.

| Name | Default | Meaning |
|---|---|---|
| `TransactionLog` | 2 | 2 history written into the file; 1 session only; 0 log off (A/B checks) |
| `TransactionLogIdentity` | off | record OS user and host names in sessions |
| `TransactionLogDerived` | 1 | derived values: 0 none, 1 cache tier, 2 durable |
| `TransactionLogSnapshotTransactions` | 200 | an unnamed version every N commits; 0 none |
| `AutoSaveEnabled`, `AutoSaveTimeout` | on, 15 min | the time rule for snapshots (AutoSaver itself writes nothing for a log-on document) |
| `TransactionLogKeepVersions` | 5 | unnamed versions kept; 0 all |
| `TransactionLogKeepNamed` | 10 | named versions per branch a saved file carries; 0 every named; below 0 the whole history |
| `TransactionLogCacheSize` | 256 | cache-tier budget, MB; 0 none |
| `TransactionLogCacheValues` | 10000 | cache-tier budget, values; 0 none |
| `TransactionLogCoalesceSeconds` | 3 | fold a quick repeat edit of one property into its step; 0 never |
| `TransactionLogCompactSize` | 256 | KB of unreferenced file-scope state that triggers compaction; 0 never |
| `TransactionLogDeltaHops` | 8 | reverse-delta chain length; 0 all full |
| `TransactionLogDeltaRatio` | 50 | largest patch kept, percent of the full size |
| `TransactionLogVerify` | off | composed snapshots compared with a real save (on in debug builds) |
| `TransactionLogPullOnSave` | on | a save pulls and merges what another process saved |
| `ClosePinnedVersion` | 0 | a released pinned version: 0 ask, 1 close, 2 keep |
| `MergeConflictSide` | 0 | the side a conflict starts on: 0 ours, 1 theirs, 2 asked |
| `TransactionLogMergeFacePaint` (string) | `ours` | element looks changed on both sides: `ours`, `theirs`, `asked` |
| `ViewObjectTransaction` | off | with the log on only decides whether restore applies view providers |
| `TransactionOnRecompute` | off | ignored with the log on |
| `StableShapeBytes` | on | saved shape bytes independent of later builds |
| `FolderSaveSync` | on | flush a folder save before its commit record |
| `FileOpenerExpiryDays` | 30 | an open-marker line of another computer not seen (opened or saved) for this many days is taken as gone; 0 keeps every line |
| `BlobCompression` | 0 | a new document's blob entries: 0 zstd, 1 deflate level 1 |
| `BlobAppendSize` | 1024 KB | pack-store append segment bound |
| `NoPartialLoading` | off | read pinned versions (and linked files) whole |
| Part `ImmutableShapeValues` | on with the OCCT fork | freeze shape values |
| Part `ShapeDiffInlineFaces` | 300 | faces above which the material diff runs on a thread |
| `Preferences/SceneShare` `UploadLimitMB`, `RequestsTotalMB` | 16, 64 | a sent file's size limit (up to 1024); all waiting sent files |

### 13.3 Environment variables

| Variable | Effect |
|---|---|
| `FC_TXNLOG_NO_STREAM` | do not stream after values at commit (measuring) |
| `FC_TXNLOG_CHECK_COPIES` | compare adopted log copies with live properties |
| `FC_TXNLOG_TRACE_IMPLICIT` | print stacks where implicit transactions open |
| `FC_TXNLOG_IMPORT_STOP_AT` | stop a fork import at a row (tests) |
| `FC_TXNLOG_CHECK_FOLDER` | compare an updated `History.db` with a whole copy |
| `FC_FOLDER_SAVE_CHECK` | write every object and compare with what would have been kept |
| `FC_FOLDER_BLOBS_COPY` | copy folder blobs instead of reading them in place |
| `FC_ELEMENTMAP_CHECK` (`=abort`) | report an edit of a held element map |
| `FC_TXN_MEASURE`, `FC_TXN_MEASURE_DUMP` | transaction cost measurement |
| `FC_SERVE_IDENTITY_DOOR`, `FC_SERVE_IDENTITY_HEADER`, `FC_SERVE_TRUST_PROXY`, `FC_SERVE_INVITE`, `FC_SERVE_UPLOAD_MB`, `FC_SERVE_REQUESTS_MB` | the served door and upload limits |

### 13.4 Python API

Document methods unless said otherwise; each has its docstring.

| Area | Names |
|---|---|
| Rows and values | `getTransactionLog`, `getTransactionOps`, `getTransactionValue`, `getTransactionValueAt`, `getTransactionCursor`, `resolveTransactionLog`, `compareTransactionStates`, `getTransactionSessions`, `getTransactionSave` |
| Undo | `undo(skip)`, `redo(skip)`, `undoTransactionFromLog(seq or [seqs])`, attributes `UndoRefusal`, `UndoNames`, `RedoNames` |
| Versions | `getTransactionVersions`, `snapshotTransactionLog`, `nameTransactionVersion`, `restoreTransactionVersion`, `openTransactionVersion`, `openTransactionState`, `saveVersionAsFile`, `saveToLog`, `getHistoryRetention`, `setHistoryRetention`, `evictTransactionCache`, `compactFileState`; `FreeCAD.openFileVersion`, `FreeCAD.closeReleasedVersions` |
| Branches | `getTransactionBranches`, `createTransactionBranch`, `switchTransactionBranch`, `renameTransactionBranch`, `trimTransactionBranch`, `deleteTransactionBranch`, `squashTransactionVersions`, `openTransactionBranch`, `getTransactionBranchState` |
| Merge and pick | `previewTransactionMerge`, `mergeTransactionBranch`, `previewTransactionMergeResult`, `previewTransactionPick`, `pickTransactions`, `previewTransactionPickResult`, `previewTransactionRollBack`, `rollBackTransactionMerge` |
| Kept values | `keepTransactionValue`, `keepTransactionValuesOf`, `releaseTransactionValue`, `getKeptTransactionValues`, `getKeptTransactionObjects` |
| Forks and requests | `getTransactionForkBranches`, `importTransactionFork`, `getTransactionRequests`, `keepTransactionSentFile`, `getTransactionSentFiles`, `dropTransactionSentFile`, `writeTransactionSentFile`, `importTransactionSentFile` |
| Linked sets | `exportLinkedSet`, `planLinkedSetImport`, `importLinkedSet`, `mergeLinkedSet`, `previewLinkedSet`, `getLinkedSetImports` |
| Files and processes | `saveCopy(path, withHistory)`, `isFileChangedElsewhere`, `getFileOpeners`, `getLastSavePull`, `getLastSaveHistory`, `getLastFolderSave`; `FreeCAD.recoverDocument` |
| Users and access | `pushTransactionActor`, `popTransactionActor`, `transactionLogin`, `transactionLogout`, `setTransactionAccess`, `clearTransactionAccess`, `getTransactionAccess`, `addTransactionGroup`, `setTransactionUserGroup`, `getTransactionUsers`, `requestTransactionMerge`, `getTransactionMergeRequests`, `closeTransactionMergeRequest` |
| Objects | `obj.ObjectUid`, `obj.pinLink`, `obj.unpinLink`, `obj.getLinkPin`, `obj.getOwnedObjects`, `doc.copyObject(..., assign=False)` |
| Part and view | `Part.getTransactionShape(doc, ref, tag)`, `Shape.diff`, `Shape.diffMaterial`, `vobj.showValueDiff`, `getValueDiff`, `hideValueDiff` |
| Gui serving | `Gui.serveBranch`, `Gui.serveRequests`, `Gui.serveDropRequest`, `Gui.serveGrants`, `Gui.serveSetGrants`, `Gui.serveDevices`, `Gui.serveSetDevices`, `Gui.serveClients` |

### 13.5 Tests

- Python (`src/Mod/Test/Document.py`): `TransactionBranchCases` (most of the
  log: undo over the log, versions, branches, merge, pick, pins, forks, sets,
  UIDs), `FolderSaveCases`, `TransactionTwoProcessCases`,
  `TransactionAuditCases` (sec 39), `UndoRedoCases`; Part
  `parttests/ElementAppearanceTest.py` for look merges.
- C++: `tests/src/App/TransactionLog.cpp`, `FileElsewhere.cpp`,
  `TransactionMeasure.cpp`; `tests/src/Mod/Part/App/ImmutableShape.cpp`.
- GUI (`tests/gui/`): `save-merges-another-process.py`, `serve-log*.py`,
  `serve-undo-redo.py`, `export-linked-set.py`, `import-linked-set*.py`,
  `set-import-marks-modified.py`, `sketch-merge-external.py`,
  `recovery-dialog-check-all.py`, `pd-undo-step-names.py`.
- Check scripts (`scripts/`): `transaction-log-*-check.py` (view, undo,
  branch, merge, mergeresult, pin, recovery, folder, frozen, import, request,
  access, author, letgo, paint, shapediff, squash, tree, twodoc, version,
  hidden), `transaction-log-view-audit.py`, browser drivers
  `logpanel-drive.js`, `logacts-drive.js`; `transaction-log-scale.py` for
  scale.
- Discipline: every gate runs with `ImmutableShapeValues` on and off; folder
  gates run with `FC_FOLDER_SAVE_CHECK`. How to run each suite:
  `docs/Testing.md`.

## 14. Open, deferred and withdrawn

**Not built, deferred.**
- Imports with a source and recipe, `ImportId` stamps: never built; an
  imported body's geometry stays durable in its `create` op (Journal 10.1,
  37.2).
- Large values as files beside the store, the 64 KB inline threshold: not
  built; a pack store is a separate design (23.1; `docs/FileBlobsManager.md`
  sec 15).
- Byte or cost limit on unnamed versions: count only (16.3, 37.13).
- Per-type sketch delta codec: the generic zstd delta only (9.3, 23).
- Camera and view state on a per-view session stack: not built; the camera is
  lost on recovery since the last version (24.10, `docs/MultiViewEdit.md`).
- Restore applies view providers only under `ViewObjectTransaction`, a switch
  always: left inconsistent (24.8).
- Cross-document transactions with a closed member: detected, not repaired
  (16.5).
- Selective undo ignores touched state; squash rows carry no before bits; a
  static property's status bits are not logged; an explicit `touch()` that
  changes nothing is not persisted (27.58, 27.63, 27.72).
- A live store shared by processes, reserved id blocks (29.7, 27.41).
- Fast-forward is several store changes, not atomic; no single-step undo of a
  fast-forward; a branch cannot push into a target no document holds (30.7).
- Clients of a shared session as separate writers with edit sessions of their
  own (30.1; `docs/ThinClient.md` 8.11).
- Per-user undo stacks last as long as the document, not the server process
  (30.10).
- More than one merge base; 3D comparison of whole branches (28.6).
- Value-merge gaps: hand on one side and expression-bound on the other; a body
  edited on two branches conflicts on `Tip`; merged or multi-line sheet cells,
  widths; third-copy round trips asked whole; wildly different clocks decide
  "last writer" wrongly (31.34).
- A pick does not check that a left-out creation is needed by a taken link
  (31.12).
- Fork pairing: kin renumbered by paste come as new objects; two renamings of
  one type in a row; squashed rows' objects (30.34, 30.22).
- Element maps: in-place edits other than names are only reported by
  `FC_ELEMENTMAP_CHECK`; tracing stops at the `;:X` marker; a `TransactionLogVerify`
  scan of string sets; a number inside a string's own text (31.33, 27.78,
  27.51, 33.7).
- Shape diff: edges and vertices unmeasured; two different objects not
  compared; the browser asks only through the edit op (32.x).
- No browser op opens a document; a view-only client cannot see the log
  (32.25).
- A grant asking a name is not matched by a plain request; `/blob` is cached
  `public, immutable` (30.40).
- Linked sets: branch tips read in part (G3, Q7 ruled not built); import and
  merge not recorded as macros (G6); zipped export; joint references and solved
  placements across a merged part (L9); no semantic assembly merge (35).
- A pinned version's materialisation still writes every entry (35.8).
- `_ObjectUUID` alone in an upstream clipboard does not bring a removed object
  back; split-XML layout does not adopt it (36.6).
- App entry points (delete/rename branch, trim, squash, name version, reject
  request, access) are not guarded on a pinned document; the panel greys them
  (37.8).
- The tree's drag does not refuse owned elements in general (37.12).
- WAL: one SQL transaction per worker drain, wanted only if flash wear matters
  (browser OPFS, phones) (37.6).
- Folder: recovery from the document's own folder or from the store without
  materialising (R1, R2); reopen after "saved while read"; a zipped folder;
  the history kept live in the folder (opt-in, local disks only) (38.9,
  38.17).
- Folder save cost: floating retention (A), C-G not built; F (~10 ms) is the
  one cheap step left; a kept object's `beforeSave` still runs (38.18-38.20).
- Folder audit items recorded until a case shows: VRML resources of a kept
  object, marker timing, trust in `Document.xml`'s stamp alone, the
  `FileBlob::_path` race, macOS `F_FULLFSYNC`, Windows rename against a reader,
  no file id in `DiskStamp`, case-only names, `HasherIndex`, nested map
  numbering (38.21).
- Windows: the lock, marker and folder code are largely unmeasured there
  (29.8, 38).
- The audit of 2026-10-27 left some fifteen small findings for a case: a
  failed branch switch clearing the undo stacks, a version document's first
  recompute row, `commit()` ignoring a refused roll-forward, `canonicalPath`
  with two missing parents, a set recorded merged after one file's merge
  threw, folder-document sets, and others (Journal 39.2; 39.3 fixed five
  of them and two panel costs).
- Held until a real use case (user, 2026-10-17): Scion's ideas -- derived
  per-row indexes and blame, a kind on every change with approval rules,
  agent provenance, rebase on the host, write grants scoped to a body (34).

**Decisions that may be revisited.**
- A merge that changes nothing is no undo step: the assistant's call, the
  user's to overrule (31.14).

**Withdrawn or reversed.**
- The log as a setting with an off mode (13.3, 14 -> 22.1); `local` mode
  (13.3).
- A branch per writer merged after every operation (17.5, 29.2-29.6 -> 30.1).
- Restarting at version 1 on a guard mismatch (16.6 -> 26.4).
- A version as a stored FCStd; the `value`/`manifest` tables (16.1, 21 -> 23).
- The scratch-document restore (27.58, 27.60); the per-branch id stride
  (27.42).
- "No store schema, no migration" (27.55 -> user, 2026-10-19).
- Schema-4 content inside the log (27.61 -> 27.62).
- A recompute as an undo step of its own (24.1 -> 33.4); view state as undo
  state only under `ViewObjectTransaction` (24.9 -> 24.10).
- Automatic pin of a link to an editable instance (27.22 -> 27.23); "nothing
  closes headless" (27.30 -> 27.39).
- `TransactionLogMergeFaceLoss` and face lookup at merge time (31.17, moot by
  names written at write time).
- A token per invitation (30.31 -> 30.32).
- A delta for values under 128 bytes (P5, user, 2026-10-22: no); a generic
  "remove what it owns" (37.10, no).
- Kept values of a closed file (32.34, user, 2026-10-15: not wanted).
- Names by content hash for every folder file; a temporary-file rename as the
  only commit; `History.db` appended in place (38.5 -> 38.8, 38.9).
- A working folder for archive documents (Q7, user, 2026-10-26: "looks
  complicated and risky") (38.17).

## Appendix: the journal's sections

| Sec | What | Here |
|---:|---|---|
| 1-7 | the idea as recorded, open questions, what it is not | 1 |
| 8 | design summary | 1, 2 |
| 9 | the record model | 2.1, 3.1 |
| 10 | derived values and the filter | 2.2, 3.4 |
| 11 | pseudo transactions | 2.1 |
| 12 | undo over the log (first design) | 4 |
| 13 | the store, the embedded copy, modes | 3.5, 3.8 |
| 14 | performance and migration | 3.3 |
| 15 | phases | -- |
| 16 | versions, retention, trim, pinned links | 5, 6.3, 10.1 |
| 17 | branches, merge, concurrent writers (first design) | 6, 7, 11 |
| 18 | audit rulings of 2026-09-22 | 1, 10 |
| 19 | survey sources | 1 |
| 20 | phase 0 measurements | 3.3 |
| 21 | phase 1 as built: logging, worker, panel | 2, 3.3, 12 |
| 22 | the end state | 1, 12 |
| 23 | entities, deltas, immutability, the shape freeze | 3.1, 3.2 |
| 24 | undo over the log as built; view providers | 4, 2.4 |
| 25 | crash recovery; streaming | 8, 3.3 |
| 26 | branches as built; trim, delete, squash | 6 |
| 27 | pinned links, file history, file-scope state, strings, touched state, schema 5, scale (and an OCCT thickness detour, 27.88-27.108, now kept in the occt repo) | 3.6, 3.7, 4.6, 5.2, 10.1, 10.2 |
| 28 | merge | 7.1 |
| 29 | concurrent writers (withdrawn); two processes on one file | 9.2, 11 |
| 30 | shared head, authors, users, forks, requests, row identity | 10.3, 10.4, 11 |
| 31 | merging inside a value, followed values, element-map threads | 7.2, 7.3, 2.2, 3.2 |
| 32 | shape diff, previews, kept values, browser History, access | 7.4-7.6, 11 |
| 33 | what the LinkVibe merge exposed: undo steps, refusals, multi-row undo | 4.2-4.4 |
| 34 | Scion (held) | 14 |
| 35 | linked sets | 10.5 |
| 36 | object UIDs | 10.6 |
| 37 | audit; storage growth and retention; WAL; paste clash; owned objects; cache budget | 3, 10.7 |
| 38 | a document as a folder; two-process audit | 9.3 |
| 39 | audit of 2026-10-27 | folded in |
