# Worker processes -- a general facility, the Animation its first use

Status: design accepted 2026-11-16, every decision as recommended (sec
9). Built on branch `Worker`: phase 0 (`docs/Animation.md` sec 4.2),
phase 1 (sec 10), phase 2 (sec 11, 12), phase 4 (sec 13), phase 5a (sec
14); the order of what is left, as ruled 2026-11-16: phase 4, phase 5,
then a reduced phase 3. Since: string ids leased (sec 15), phase 5b (sec
16), phase 6, a recompute made by a worker (sec 19); results no longer
depending on what was shown, and a worker's names spelled as its
client's (sec 20); an export made by a worker, pinned to the design as it
was asked for (sec 24); a geometry check made by a worker (sec 25); the
worker of a long job that was dropped ended (sec 26); what a worker says
through its Console said here (sec 27); an edit's long recompute made by a
worker (sec 28); a worker keeping what it made as it moves across it (sec
29); what a recompute took read from the log after an open (sec 30); the
recompute at the end of a task panel's or a sketch's edit (sec 31).

Companion to `docs/ComputeBoundaries.md` (Boundary B, whole-document
workers first) and `docs/TransactionLogJournal.md` (the log this builds
on; cited as "log sec").

## 0. What phase 0 taught

The Animation-only build (`App/AnimationWorkers.*`) works and halves a
filleted plate's first pass (18.5 s -> 9.5 s, 60 frames, mini). It also
shows where a general facility has to be different:

| Phase 0 | Cost or flaw | Here |
|---|---|---|
| The document by `saveCopy()` | goes through the save path: the design put back and recomputed, the frame on show remade, the cache's own disk frames dropped (~0.3 s, and lost work) | a log snapshot, read by the workers from the GUI's own store (sec 3) |
| A worker per job, killed at any edit | every edit pays a restart and loses the workers' warm state | workers that move along the log to the next version (sec 4) |
| Answers as lines on stdout | a progress count printed by OCCT/FreeCAD glued onto an answer and lost it | a socket of its own (sec 2) |
| A file per frame, its string table beside it | the strings read back by content, a frame's file parsed into the document before it can be shown | results by content hash in a shared arena; string ids from reserved blocks (sec 5) |
| Shown only after the shape is parsed and the view rebuilt | the floor of a pass: ~150 ms a frame read back, whatever the workers do | a prepared display form drawn without materialising the document (sec 6) |
| Animation code | none of it reusable | a pool, jobs and results that any client uses (sec 1, 7) |

## 1. Layers

```
  client          Animation (frames), later: recompute, mesh, export, check
  ----------------------------------------------------------------------
  jobs            App::WorkerJob: kind, version, inputs, priority; result
  ----------------------------------------------------------------------
  results         values (the log's CapturedValue), blobs by SHA-1 in a
                  shared arena, prepared display chunks
  ----------------------------------------------------------------------
  document        a log version of the live document; workers read the
                  GUI's store; move along it on edits
  ----------------------------------------------------------------------
  processes       App::WorkerPool: spawn, socket, health, size, kill
```

Each layer is usable without the ones above it, and each phase below
lands one of them under the Animation, which keeps working throughout.

## 2. Processes: `App::WorkerPool`

- A worker is `ThunderealCmd -c "from freecad import worker; worker.serve()"`
  with its own copy of the settings (as phase 0). Python features run in
  it, so it has the GUI's trust: the same expression-security grants and
  policy file, passed on the command line.
- **Control on a local socket** (`QLocalServer`/`QLocalSocket`: a Unix
  domain socket, a named pipe on Windows), the name passed in the
  environment. Messages are length-prefixed: a small JSON header and an
  optional binary body. stdout and stderr go to the worker's log file,
  never parsed. This removes the whole class of bug phase 0 hit.
- **Lifetime:** a pool per document (not per job), started on first
  demand, idle workers ended after `WorkerIdleSeconds` (30). A worker that
  dies fails its job; the job goes back to the queue once, then to the
  client as failed. A job past its deadline (`claim` waited 30 s, or
  twenty times the kind's mean) gets its worker killed.
- **Size:** start one; add one only when a client is waiting on a job a
  worker holds while others are queued (the consumer is starved), and
  the memory available leaves a seventh of the machine after the
  largest worker and a quarter. Phase 0 grew on memory alone and started
  seven where two were enough. Cap `Workers` (-1: cores less one, up to
  8; 0: off).
- **Without processes** (the wasm tier, `Workers` 0): the pool says so
  and every client keeps its in-process path. No client may depend on
  workers for correctness.

## 3. The document a worker holds: a log version, read from the GUI's store

- **Taking it.** `Document::snapshotToLog()` (log sec 16, 23): the live
  document serialised to the log with no save signals, no archive, no
  string compaction, no deactivation of an Animation's pose -- the
  objects' `beforeSave` and the writing of what the log does not hold
  yet. Measured 85-114 ms on the main thread for 200-600 objects (log
  sec 38.17), against phase 0's save. It refuses while a transaction is
  open; a pool waits for the commit (and an Animation's writes are not
  transactions).
  - An Animation's pose: the log already records a posed property as its
    rest value (`TransactionLog.cpp:4188`, `:4327`,
    `Animation::heldRestValue`), so the keyed inputs of the version are
    the design. What is derived from the pose -- a shape recomputed for
    the frame on show -- is not a rest value and goes in as it is; a
    worker recomputes every such value for its frames anyway, since its
    first evaluation touches what the tracks write. To be proved by a test
    in phase 2 (a version taken mid-play, a frame from it equal to the
    frame made in process), not assumed.
- **Reading it from another process: multiple readers.** Not built today
  (log sec 29.7: each process has its own store; shape (b), one live
  store, is designed and not built). This needs the read-only half of
  (b), the GUI the only writer:
  - `FileHistory::attachReader(store dir)` in the worker: opens the GUI's
    `log.db` read-only through WAL (not `immutable=1`, which assumes no
    writer), and its `blobs/` in place, as a folder document reads saved
    blobs (log sec 38.9).
  - `Document::openFileVersion(history, num, frozen=false)` there, with
    the blobs taken from the store by hash (log sec 27.32: no disk round
    trip).
  - A **reader lease** in the store: a row per worker naming the oldest
    version it holds. While one stands the GUI does not prune a blob or
    entity that version reaches and does not reuse a blob name for other
    content (the folder rule of log sec 38.9, extended to the store).
    WAL checkpoints are safe with readers; a truncating checkpoint is
    deferred while a lease stands.
  - The worker never writes the store.
- **Fallback** where the store cannot be shared (a read-only medium, a
  remote worker later): `materialiseVersion` into a job directory with
  the blobs copied, the worker restoring that. Slower, same contract.

## 4. Staying in step: moving along the log

- An edit commits a transaction; the GUI takes a version at the pool's
  next use (not at every commit), and tells the workers its number.
- A worker moves its document to it with the log's in-process machinery
  (`_moveAlongLog`, log sec 27.34: rows applied by fold), reading the new
  rows through its reader connection. It recomputes nothing on the move:
  values the rows carry are restored, owners of what changed are
  touched, as a checkout does.
- A job names the version it is for. A job queued for an older version
  is dropped when its inputs changed (the client says which objects a job
  reads; the Animation: the frame's scope), and kept when they did not.
- Phase 0 killed every worker at every edit; this keeps them warm and
  keeps the frames an edit does not reach, as the in-process cache does.

## 5. Results: by hash, in a shared arena; strings from reserved blocks

- **Values** are the log's `CapturedValue`: the property's fragment, its
  attachments, the blobs it names. Geometry (a shape's binary BRep, its
  element map) goes out as blobs named by SHA-1.
- **The arena** is a set of files in the pool's directory, mapped by both
  sides (`mmap`, `MapViewOfFile`; a file, so it is portable, outlives a
  crash for diagnosis, and the OS pages it): a blob is written once under
  its hash and never changed. The GUI adopts a result's blobs into its
  `FileBlobManager` by hash -- a link or rename, no copy -- so a frame on
  disk is the store's own blob, deduplicated across frames (an object a
  frame did not change is the same blob) and across workers.
  - What the arena saves: the copies and the file I/O. What it does not:
    parsing a BRep (38 ms of a filleted frame) -- OCCT has no form that
    is read in place. That is sec 6.
- **Strings.** A worker's hasher starts as the version's table (read from
  the store, log sec 27.50). Each worker mints new ids from a block the
  GUI reserves for it (`StringHasher::reserveBlock(n)`; the allocator
  reservation log sec 29.7(b) proposes), so an id is the same string in
  both processes. A result carries the strings its values added; the GUI
  takes them in under the same ids (`StringHasher::merge`, which exists:
  "each id it has that this one has not, under the same id"). This
  replaces phase 0's import by content (`RestoreStrings`), which rewrites
  every element name of every value read back.
  - Built 2026-11-16 as sec 15: the ranges leased by the GUI's table
    (not fixed blocks), the strings taken in with `takeRows` (all or
    none; a string held under another id refuses), the import by content
    kept as the fallback.
  - Object ids are the version's; a job may not create objects. Minted
    numbers (`RestoreMinted`, log sec 31.14) are the same rule: a job that
    would mint is refused, or falls back to the import scopes.

## 6. A prepared form for display

The floor of a phase 0 pass is the GUI parsing each frame's shape and
rebuilding its view: ~150 ms of the filleted frame's 307. More workers do
not move it. Only not doing it does.

- **The form exists:** the scene stream's content-addressed object
  manifests and mesh chunks (`docs/SceneStreaming.md` sec 2-4), which the
  browser viewer draws with no document at all, and which the browser's
  bake already sequences per frame (`MotionClip`).
- **The producer is the problem:** today chunks come from the Coin
  traversal of a 3D view (SceneStreaming sec 2.1: "a headless source ...
  not built"). A console worker has neither. Two ways:
  - (a) **an App-side mesh writer** for Part shapes: faces and edges
    tessellated at the deflection the view uses (`Part::meshShape` with the
    view provider's `Deviation`/`AngularDeflection` passed in the job),
    written in the mesh chunk layout, keyed by content. Appearance stays
    the GUI's (it does not change per frame; a frame that changes a look
    says so and is drawn the slow way).
  - (b) **a headless Gui worker**: the GUI binary without a window,
    producing chunks through the real traversal. Exact, but a much larger
    process and the "headless source" work besides.
  Recommended: (a), for Part features only; anything else falls back.
- **Use:** while the Animation plays or exports, a frame a worker made is
  drawn from its chunks -- the renderer substitutes the object's draws,
  as the browser viewer does -- and the document is materialised to the
  frame only when it has to be: on pause and stop, on a pick or a
  selection, before anything reads a property (a Python call, an
  expression, the property editor), and for an export path that needs
  shapes (Cycles reads the scene; checked at that phase). The values are
  in the arena already; materialising is the restore phase 0 does at
  every frame.
- The browser's bake takes the same chunks, so preparing a served
  animation stops running the GUI's capture per frame.
- This is the one change of behaviour: during playback the document
  lags the picture. It needs your ruling (sec 9, D5).
- As built (sec 16): not chunks drawn by the renderer. Measured, the time
  of a frame shown was in the document taking in the geometry no view
  draws -- a chain's hidden features -- not in drawing the one shown; the
  view draws from the shape as before, and only what it does not draw is
  held back.

## 7. Jobs

```cpp
struct WorkerJob {
    std::string kind;            // "animation.frame", later "recompute", "mesh" ...
    long version;                // the log version it is for
    nlohmann::json inputs;       // the kind's own
    std::set<std::string> reads; // objects whose change makes it stale
    int priority;                // claimed > visible > queued
};
struct WorkerResult {
    bool ok; std::string error;
    std::vector<Value> values;   // object, property, CapturedValue
    std::vector<std::string> blobs, chunks;  // hashes in the arena
    std::vector<StringEntry> strings;        // added, by id
    double seconds; std::size_t resident;
};
```

- Kinds are registered on both sides: a C++ handler or a Python one
  (`freecad.worker.register("animation.frame", fn)`), so a workbench can
  add its own.
- `pool.submit(job)`, `pool.claim(job)` (wanted now: a queued job is
  handed back for the client to do itself, a running one is waited for),
  `pool.cancel(predicate)`, `signalResult`. Exactly-once is the pool's
  rule, as phase 0 proved it for frames.
- A worker answers `unfit` for a job it cannot do faithfully (a Python
  class it cannot import); the client does it in process.
- Kinds after the Animation, in the order they pay: `recompute` (an object
  set at a version: ComputeBoundaries' process-per-document rung, a long
  recompute without freezing the GUI), `mesh` (tessellation of large
  shapes for the view), `export` (STEP and mesh export), `check`.

## 8. The Animation on it

- `prepareFrames()` submits `animation.frame` jobs for the frames not
  kept, `reads` each frame's scope; `evaluateAt` claims the frame it
  shows.
- A result is a frame on disk whose values name arena blobs; with sec 6,
  a frame also has display chunks and is drawn from them while playing.
- An edit drops the frames it reaches (as now) and only the jobs whose
  `reads` it reaches; the workers move to the new version.

## 9. Phases and decisions

| Phase | Lands | Removes from phase 0 |
|---|---|---|
| 1 | `WorkerPool`, socket protocol, job registry, `freecad.worker`; the Animation ported, same behaviour | stdout parsing; Animation-only code |
| 2 | snapshot + read-only reader of the GUI's store, leases; `openFileVersion` in the worker | `saveCopy`: the save path, the remade frame, the dropped disk frames |
| 3 | arena, blobs adopted by hash, string blocks + merge (strings built: sec 15) | per-frame files, import by content |
| 4 | move along the log on edits; jobs dropped by `reads` | restart at every edit |
| 5 | App-side mesh chunks; frames drawn from them; lazy materialise; the bake takes them (as built: sec 14, 16) | the read-back floor |
| 6 | `recompute` as the second client | -- |

Each phase is measured on the plates of `docs/Animation.md` 4.1/4.2 and
gated by `TestAnimation`, `tests/gui/animation-workers.py`, and a new
`TestWorkerPool` (the pool alone: lifecycle, exactly-once, a worker
killed, a version moved, a lease holding back a prune).

Decisions (taken 2026-11-16: each as recommended):

- **D1 Control channel**: local socket (recommended) or keep stdout lines.
- **D2 Multiple readers**: build the read-only half of log sec 29.7(b) --
  workers reading the GUI's live store, leases holding back pruning --
  as the way a worker gets the document (recommended); or materialise
  each version to a directory (simpler, slower, no shared store).
- **D3 String ids**: reserved blocks and `merge` (recommended), or keep
  import by content.
- **D4 Arena**: file-backed mapped files in the pool's directory
  (recommended), or anonymous shared memory (`shm_open`/`memfd`, Windows
  named mappings): a little faster to set up, gone with a crash, and the
  blobs then have to be copied into the store to outlive the pool.
- **D5 Display without materialising** (sec 6): the document lags the
  picture during playback and export, caught up on pause, pick or read.
  And its producer: App-side mesh chunks for Part shapes (recommended),
  or a headless Gui worker.
- **D6 The second client** after the Animation: `recompute`
  (recommended), or another.

## 10. Phase 1, as built

(2026-11-16, branch `Worker`: `App/WorkerPool.{h,cpp}`,
`Ext/freecad/worker.py`, `App/AnimationWorkers.*` ported onto it;
`Mod/Test/TestWorkerPool.py`.)

- **The pool.** `WorkerPool::of(doc)`, one per document, gone with it
  (`signalDeleteDocument`: its workers killed, its directory removed, no
  client told). `start()` takes the copy if none stands -- still phase 0's
  `saveCopy()`, into `<temp>/Workers-<pid>-<n>`, with the settings and the
  list of objects with a Python class -- and starts one worker.
  `submit(Job)` -> id; `claim(id)` -> `Yours` (it was queued: the client
  does it), `Done` (it was running and its result has been handed over),
  `Failed` (its worker died, or ran past 30 s or twenty times the kind's
  mean: the client does it); `state(id)`; `cancel(tag predicate)`;
  `stop(why)`; `poll(ms)`; `wait(seconds)`; `info()`;
  `signalChanged`. A job's `done(Result&)` is called once, unless the
  client claimed it while queued or cancelled it; the result's `file`,
  written by the handler in the pool's `out/`, is the client's to move,
  else deleted.
- **The socket.** `QLocalServer` with `UserAccessOption`, its name and a
  random token in the worker's environment; a worker's first message,
  `hello`, carries the token and its pid, and a connection without both
  is closed. Messages are a u32 header length, a u32 body length, a JSON
  header and a body (none yet). The pool says `setup`, `job`, `quit`; a
  worker `hello`, `ready` or `unfit`, `result`. A worker's standard output
  and error go to `worker<n>.log` in the pool's directory and are never
  read. `App` now links QtNetwork.
- **The worker's loop** is C++ (`WorkerPool::serve()`, reached as
  `freecad.worker.serve()` -> `FreeCAD._serveWorker()`): blocking reads on
  its socket, the copy opened with undo off, a job run by a C++ handler
  (`WorkerPool::registerKind`) or a Python one (`freecad.worker.register`,
  the job naming the module that registers it), its answer with the
  seconds and the bytes the process holds. A Python handler raises
  `freecad.worker.Unfit` for a job it cannot do faithfully; a kind no one
  registered is unfit too. The pool going ends the loop.
- **Lifetime.** A worker that dies has its job queued once more, first in
  line; twice, the job fails. Three deaths or failures in a row stop the
  pool. An idle worker is told to quit after `IdleSeconds` (30; 0: at
  once); the copy stands, and the next job starts a worker on it.
- **Staleness, until phase 4.** The pool watches the document: a write
  that changes its file (`WriteEffect::Dirty`), not inside a recompute,
  not an Animation's (`WriteCause::Animation`), and not of an Animation
  object (whose frames it says itself, `Animation::designChanged()`), stops
  the pool: every job handed back `Cancelled`, the copy dropped.
- **Growth** as sec 2: one at a time once every worker has done a job,
  memory allowing; with a client claiming (a claim in the last two
  seconds) only when it had to wait or take a queued job back; with none,
  while more is queued than there are workers.
- **Python**: `App.workerPool(doc)` -> `start()`, `submit(kind, inputs,
  module, tag)`, `claim`, `state`, `result(id)` (once: `Status`, `Error`,
  `Output`, `File`, `Seconds`, `Resident`), `cancel(tag)`, `stop`, `poll`,
  `wait`, `info()` (`Started`, `Workers`, `Limit`, `Spawned`, `Busy`,
  `Queued`, `Resident`, `Done`, `Failed`, `Reason`, `Directory`, `Pids`).
- **Preferences/Workers**: `Workers` (-1 auto, 0 off), `Command`,
  `KeepJob`, `IdleSeconds`. The Animation's `Workers` moved here;
  `WorkerMinSeconds` stays the Animation's.
- **The Animation** submits a job per frame, tagged as its own, the
  pool's `tables/` for the string tables (phase 3 replaces them); its
  `claim` before a frame is made here, its callback moving the frame into
  the cache directory and adopting it when safe. Cache or Active off,
  `clearCache()` and `stopWorkers()` cancel its jobs only.

Traps met building it, each with the event loop in the GUI:

- `waitForReadyRead()` on a socket emits `readyRead` inside the wait, so
  the slot ran first and handed the new connection over; reading it again
  after the wait read a moved-from socket (a crash in the first run).
- Accepting a connection by hand (`waitForNewConnection`) while an event
  loop runs leaves the server's own notifier to call a blocking
  `accept()` for a connection already taken: the GUI hung in it. The pool
  accepts by hand only where no loop runs (`loopLevel() == 0`, a console).
- A client waiting inside a result's callback reads the same worker
  again: a read is safe to nest, a message being taken off the buffer
  whole, and a guard against nesting made that wait spin to its deadline.

Measured 2026-11-16 on mini, the manual's plate (36 holes filleted, 134
faces, 41 frames, `core/animation/workers.py`): made here 5.6 s; by the
pool 2.3 s for the 40 not kept, five workers at the peak holding 72 MB
each, none made here, none failed (phase 0: 2.5 s). Tests: TestWorkerPool
12, TestAnimation 70, `tests/gui/animation-workers.py` 19, and
animation-cache 27, animation-timeline 50, serve-animation 62,
animation-export-clock 12 unchanged; ctest 1268/1268 (FormWidgets failed
once under `-j 4`, passed alone).

Not done in phase 1: the expression-security grants passed to a worker
(sec 2; done 2026-11-19, sec 23); jobs dropped by `reads` (phase 4).

## 11. Phase 2a, as built: the copy is a version of the log

(2026-11-16.) Phase 2 is in two steps. 2a takes the document the workers
hold from the log; 2b (sec 12) has them read it from the GUI's store
themselves, and 2a's folder is its fallback.

- **The copy.** `WorkerPool` takes a version of the log
  (`Document::_snapshotToLog("workers")`: a row of kind `workers`, an
  unnamed version that retention lets go like any other), waits for the
  log's worker to have written it, and writes it out as an unpacked
  project in the pool's `version/` (`Document::materialiseVersion`, the
  blobs as files) with the file's string table, which a version does not
  carry (log sec 27.50). A worker opens that folder. `WorkerPool` is a
  friend of `Document` for the two. `Preferences/Workers/FromLog` false
  goes back to the saved copy, for A/B.
- **When not.** The log refuses a snapshot while a transaction is open
  (or with the log off): the copy is then saved, as in phase 1.
- **The pose.** Proved, not assumed: `testWorkersFromAVersionTakenMidPose`
  takes the version with the Animation showing its middle frame, and
  every frame the workers make from it has the shape, faces and element
  names of the frame made here. The log records a posed property as its
  rest value (`Animation::heldRestValue`), and what derives from the
  pose is recomputed by the first frame a worker makes.
- **What it saves.** On the main thread, the 64-hole filleted plate:
  `prepareFrames()` with the Animation posed 39 ms, against 420 for the
  saved copy, which puts the design back, recomputes it and makes the
  frame on show again. With no Animation, the version and its folder cost
  47-52 ms against the save's 34: the snapshot is 26 ms of it, the
  folder the rest, which is what 2b moves off this process.
- **And the frame on show** is no longer made again: a pass started at
  the rest frame now has every frame made by the workers.

Tests: TestWorkerPool 14 (the copy a version, a saved copy with a
transaction open), TestAnimation 71 (the version taken mid-pose),
`tests/gui/animation-workers.py` 19.

## 12. Phase 2b, as built: the workers read the GUI's store

(2026-11-16.) The read-only half of log sec 29.7(b), with the GUI the
only writer.

- **The store, shared.** `TransactionStore::openSQLiteReader(path)`: the
  GUI's `log.db` opened read-only through its WAL -- not `immutable=1`,
  which assumes no writer -- of this build's layout only, never migrated.
- **A reader core.** `TransactionLogCore(history, store, blobs)`: the
  counters, names and strings read from the store; no session, no
  environment row, no worker thread; a post is refused, since no worker
  would take it and the next flush would wait for ever. A blob whose
  bytes the GUI keeps in its own blob store (`enc` file: a file, or a
  member of one of its segments, which its worker repacks) is read from
  `blobs/<hash>` instead.
- **A reader history.** `FileHistory::openReader(store, blobs)`: in a
  directory of its own, registered under no file, its hasher filled from
  the store's strings -- under the GUI's ids.
- **The GUI's part.** The version is taken as in 2a; then the blobs it
  reaches (through deltas' bases and attachments too) whose bytes are
  files of this process's store are put out in the pool's `blobs/`,
  named by hash -- a hard link where the blob is a file of its own, its
  bytes where it is in a segment -- and once only: the directory lasts as
  long as the pool, so after an edit only what the edit changed goes out.
  If anything is missing, 2a's folder is written instead.
- **The worker's part.** Its setup names the store, the version and the
  blobs; it opens the reader history, writes the version out as an
  unpacked project in its own folder (`materialiseVersion`, as 2a did in
  the GUI) with the reader's string table, and opens that.
- **A lease.** `TransactionLogCore::holdVersion(num, hold)`: while the
  pool's copy stands, retention does not evict its version, so a worker
  started later -- grown, or after the idle ones quit -- still reads it.
  Released when the copy is dropped. Explicit evictions (a squash, a
  compaction) are the user's and are not held back; a worker that then
  finds the version gone answers `unfit`, and the frames are made here.
- **Settings**: `ReadStore` of Preferences/Workers (true); false writes
  2a's folder.

- **A version's claimed values, in a folder** (found by this work, fixed
  in `Document::materialiseVersion`, which every checkout shares): a
  snapshot lets the log claim a property whose value it holds (log sec
  23.3), and the files that value's Save wrote -- a fillet's edge list,
  `<FilletEdges file="Property.txt"/>` -- are attachments of the value in
  the log, not entries of the version. A folder written from a version
  had no such file: a worker's fillet restored with no edges ("Edge link
  size mismatch"). Two values captured apart may also give the same name
  -- two fillets, each "Property.txt". A composite entry is now composed
  for the folder with each part's attachments beside it, a name already
  taken made the value's own (its stem, the attachment's hash, its
  extension) in that part's fragment. In-process versions read the
  values another way and were right before.

Measured on the 64-hole filleted plate, posed: `prepareFrames()` 26 to
28 ms on the main thread -- the snapshot and the blobs put out -- against
39 for 2a and 418 for the saved copy.

What phase 3 has to gain, measured before it is built (console, the
same plate, 20 frames on disk each way, read back in turn): a frame this
process wrote 14.8 ms, a worker's -- the same format, its strings taken
in by content -- 16.7 ms. Import by content is about 2 ms of a frame's
read-back here; in the GUI the floor is the view rebuilt for the shape
(~150 ms, sec 6), which only phase 5 removes.

Tests: TestWorkerPool 17 (two fillets' edge lists, both ways; the store
read, only the edited shape's blob put out again, the version held past
retention while the pool holds it and let go after), TestAnimation 71,
`tests/gui/animation-workers.py` 19.
## 13. Phase 4, as built: the workers move along the log

(2026-11-16.) An edit no longer kills the workers. It drops the jobs it
reaches; the workers stay, and the next copy reaches them as the rows
between the two versions.

- **The edit.** The pool watches the document as before (a write with
  `WriteEffect::Dirty`, not inside a recompute, not an Animation's), and
  now an object made or deleted and a dynamic property added or removed,
  which change no value. Any of them makes the copy **stale**, and hands
  back `Cancelled` ("the design changed") the jobs the edit reaches:
  - `Job::reads` (Python `submit(..., reads=[...])`, objects or names):
    the objects the job's result is made from. A job is reached when the
    edited object, or an object that depends on it
    (`getInListEx(true)`, an expression's hidden links included), is
    among them -- the rule an Animation drops its frames by. A job that
    names nothing is reached by every edit.
  - A running job that is reached runs on; its result is thrown away.
  - What is not reached runs on, on the copy it was queued for: nothing it
    reads changed, so the copy it runs on does not matter.
  - `WorkerPool::designChanged(obj)` for a change the pool does not see:
    an Animation's tracks, start, rate (the Animation passes itself, so
    what reads its Time is reached).
- **The next copy** is taken at the pool's next use, `start()` or
  `submit()`, not at every commit. An implicit transaction (what a script,
  or a GUI event that is no command, wrote) is committed first, as a save
  commits it, so that its row is there to move along; an explicit
  transaction still open means a saved copy, as before. The version is
  taken and held, the one before let go (a worker moving from it reads
  the rows after it and the values of the new one), and only the blobs
  that are new go out (sec 12).
- **Each worker** is sent `setup` again, with `from`, `fromSeq` and
  `toSeq` when it is on a version of the same store. It reads it after
  the job it is on; a job sent after it is made on the new copy, since a
  socket keeps order. It answers `ready` with `moved` and the seconds it
  took. The old copy's directory goes once no worker is on it.
- **In the worker**, the move is the log's own checkout fold (log sec
  27.34): the reader brought up to date (`TransactionLogCore::
  refreshReader()`: the counters, the names, the strings the GUI minted
  since), a `TransactionLog` cursor at `from` over the reader, and
  `Document::_moveAlongLog(fromSeq, toSeq)`: objects made and removed,
  dynamic properties, every value that differs restored, nothing
  recomputed, the touched state as the rows end. Where the rows cannot be
  folded (a value the store no longer has), where the worker is on a
  saved copy or a folder, or where a write no transaction recorded came
  in between (undo and log both off), it opens the version whole.
  - The worker's document is now a version document of the reader's
    history (`_restoreAsVersion`, as `openFileVersion` reads one): its
    strings and blobs are the reader's, found in the store by hash, so a
    move goes on reading the same. The folder it was read from goes with
    the document.
  - A reader never writes the store: `putBlob` only holds the blob, and a
    cursor's constructor sets no meta.
- **String ids.** A worker gives new strings ids from a block the pool
  names, `2^30 + n * 2^24` for worker `n` (a string of its own marks the
  block's start; the pool names none if its own ids reach there). The
  GUI's strings read in at a move, under the GUI's ids, then never meet
  one the worker gave. This is the first half of phase 3's blocks; the
  results still come back by content.
- **Directories.** `Info::directory` is now the pool's: the settings, the
  logs, the results' files (`out/`), the Animation's string tables, the
  blobs put out, the copies -- gone with the pool. `Info::copy` is the
  copy standing (`copy-<k>`), gone at `stop()`. `Stale`, `Moves`,
  `Reopens` say what happened.
- **The Animation** gives each job its reads: what its tracks write
  (through strips, in the Animations they play), what reads its Time,
  what is touched already, and every object depending on those; none
  with `Recompute` full, which recomputes everything. An edit drops the
  frames it reaches as before; its jobs, and the frames made and not
  taken in yet, go only if the edit reaches them.

Found by this work, fixed in `TransactionLogCore::putComposite`: a save
stores each property's value as a part of its version, and the log holds
it from then on, so the next snapshot claims it and skips its save. A
list property's file -- a fillet's `Fillet.Edges.bin`, written by the
save as an entry of its own -- was not attached to the part. Every
version taken after a save, until the fillet was edited again, named a
file it did not carry: a worker's fillet, or one read from such a
version in process, restored with no edges ("Edge link size mismatch").
A part stored unclaimed now takes the files its fragment names among
the save's other entries as its attachments, as a commit's capture has
them.

Measured 2026-11-16 on mini, the 64-hole filleted plate, one worker
warm, from `submit()` to the result after an edit and a recompute (the
snapshot, the blobs, the worker's part and the job): moved along the log
91-93 ms; opened whole from a folder (`ReadStore` off) 105-119 ms; a
worker started cold for it, as phase 1 had to, 312 ms. Each answer was
the volume of the fillet here. Most of what an edit saves is the process
kept warm; the fold saves the rest of the open, and more on a larger
document, where an edit changes less of it.

Tests: TestWorkerPool 23 (an edit dropping the jobs that name nothing and
moving the worker, same process; a job an edit does not reach running
on; three edits with shapes changed, an object added and one removed,
each a move; an object added or removed alone; undo off, still a move;
from a saved copy, a reopen, the fillet's edges there; the file a save
wrote with its value), TestAnimation 72 (an edit dropping the frames,
the workers staying, moving, and their frames those made here; an edit
beside the frames leaving them), `tests/gui/animation-workers.py` 23 (an
edit dropping the frames, the workers staying, the next pass made by
them moved, its frames the edited design; the frame on show now made by
the workers too, the first copy a version).

Not done: a job does not name the version it was queued for; with
`reads` it does not need to. The results still come back as files with
string tables, imported by content (phase 3).

## 14. Phase 5a, as built: a frame meshed where it is made

(2026-11-16.) Phase 5 is in two steps. 5a takes the mesh off the frame's
read-back with no change of behaviour; 5b (sec 16) draws a frame without
materialising what no view draws.

- **Measured first.** `sample` of the GUI's main thread stepping through
  a worker's frames read from disk (the 64-hole filleted plate, 40
  frames, 1 ms samples): of a frame's replay, the view meshing the
  frame's shape (`BRepMesh_IncrementalMesh` in `updateVisual`) about 60%,
  the link element references updated for the new shape 13%, parsing
  the shape 1%. What `docs/Animation.md` sec 4 had said, that the mesh
  made again is not where the time goes, was wrong: a triangulation read
  back keeps its measured deflection, not the one it was built at, and
  with quality decrease allowed the view (and OCCT) accept a resident
  mesh only within a tenth of the ask -- so a frame written with its mesh
  was meshed again all the same.
- **The view says how it meshes.** `Animation::displayHint`, set by
  PartGui: for an object with a `ViewProviderPartExt`, "mesh <deviation>
  <angle>", as `updateVisual` takes them (the `OverrideTessellation`
  preference and the minimums applied). A hidden object too: its visual
  is built all the same when the view asks the scene for its bounds,
  which it does for every frame (a plate's hidden `Cut`, meshed so each
  frame, was a third of what was left after the first cut of this). The
  Animation's jobs carry the hints of every object (`display` in the
  job's inputs); `FrameMeshes` of Preferences/Workers (true) off leaves
  them out, for A/B.
- **The worker meshes so.** `Animation::captureForDisplay`, set by Part's
  App side: a frame's shape is meshed at the view's deflection -- of the
  posed shape's box, the view's formula -- each face's triangulation
  given the deflection it was built at, and captured with its triangles
  and normals (a thread-local switch of `TopoShape::exportBinary`, set
  for that capture only). `Animation.writeFrame(index, file, tables,
  display)` takes the hints from Python too.
- **The view finds it made**: `tessellationIsRedundant` accepts it, and
  `BRepMesh` is not entered.
- **Also fixed**: `waitWorkers()` called from a script in the GUI never
  took a worker's connection -- the server's notifier runs only in the
  event loop, which a poll did not let run. A poll that waits now
  processes the events that are no input; a poll of none (a claim inside
  an evaluation) still does not.

Measured 2026-11-16 on mini, the same plate, a frame read from disk and
the view waited for, three runs each way: the replay (Time set) 76 ms
without the mesh, 34 ms with it; the whole frame 135 ms, 74-77 ms (one
run, the first after a build, 94). A frame from memory is unchanged (56
ms, of it 18 ms replay). The frame's file is larger, 2.6 MB against 0.36
MB. Of the main thread's time on such a frame, the mesh is now 0.2%.

What is left of a frame read from disk: the replay's 34 ms (the view's
fill of its nodes, the restore, the element references, each about a
third) and the drawing after it -- of the main thread's busy time the
bgfx renderer 10%, the render cache 8%, Coin 3%, the GL surface's flush
(the frame presented) 11%, the other widgets 6%. The ruling of
2026-11-16 takes the drawing next: the meshes of the frames to come
prepared before they are shown.

Tests: TestAnimation 73 (a frame written meshed, near three times the
size, a hint it does not know written plain), `tests/gui/animation-
workers.py` 24 (a worker's frame holds the view's mesh; the pictures of
its frames those of the frames made here, as before).

## 15. String ids leased: a result's strings taken in under their ids

(2026-11-16.) The base of phase 6 (a `recompute` job), built with the
Animation's frames as its first user. It replaces the fixed blocks of
phase 4 (sec 13: `2^30 + (n % 63) * 2^24`), which only kept a worker's
own table apart from what it read in, and leaves the import by content
(`RestoreStrings`) as the fallback.

- **Why, measured first.** The import by content rewrites every element
  name of every value read back. On the 64-hole plate (sec 14) that was
  ~2 ms a frame. On a recompute-sized result it is not small and grows
  faster than the names do (`~/works/sw/fcad-probes/worker/p6probe.py`:
  a chain of successive cuts under a fillet, the plate keyed, so a frame
  is a recompute of the whole chain; 8 frames by 1 worker, read back from
  disk, against the same frames made here):

  | chain | objects | element names | worker frame | own frame | by content |
  |---|---|---|---|---|---|
  | 20 cuts | 42 | 2352 | 20.4 ms | 12.0 ms | ~8 ms (3.6 us a name) |
  | 60 cuts | 122 | 13992 | 272.8 ms | 116.7 ms | ~156 ms (11 us a name) |

  A recompute result is in the document at once and stays, so merging
  by id is also what keeps the GUI's table and the worker's the same
  table -- with the fixed blocks it could not be done safely: `insertRows`
  and `merge` set the next id past the highest held, so the GUI's next id
  would land in a worker's block, a save would write that, and a worker
  restarted on the same block would give the same ids again.
- **The GUI's table hands the ids out.** `StringHasher::reserveBlock(n)`:
  the n ids after `lastID()`, which moves past them -- never given there,
  nor after a save (the `lastid` a save writes includes them). The pool
  leases each worker a range in its first setup (`ids {first, count}`),
  16384 to start (`IdBlock` of Preferences/Workers, at least 16), doubling
  at each further ask up to 2^20: what goes unused is never given, and an
  id is 32 bits on Windows, so the ranges stay small.
- **The worker gives ids from its lease.** `StringIDLease`, shared by
  every table of the worker's copy and kept when the copy is reopened;
  `StringHasher::setLease()` makes the three places a new id is minted
  take the lease's next one (`nextID()`), whatever the highest held -- a
  move reads the GUI's newer ids in above the lease. Run out in a job,
  the worker asks (`ids`), and waits for the answer, setting aside any
  other message meanwhile (a setup can come while a job runs, sec 13);
  the GUI answers from `handle()`, which a claim's wait also reaches. No
  range to give (the counter at its end): the mint throws, the job fails
  and the client does it.
- **A result carries the strings its values name.** `captureValue`
  already lists the ids a value refers to (`CapturedValue::stringIds`);
  `StringHasher::rowsOf(ids)` gives them and everything they refer to,
  transitively -- the version's strings as well as the worker's own, so
  a string the GUI dropped since (compaction at a save) comes back under
  its id. `writeRows` / `readRows` carry them in a file (a frame's: after
  its scope, magic `FCAWFR02`; a count plus one, 0 for none sent).
- **The GUI takes them in whole or not at all.**
  `StringHasher::takeRows(rows)`: refused, nothing changed, when an id is
  held with another string; when a string is held under another id (one
  element name would be spelled two ways, and names compare as text --
  this is a string the GUI made itself after the worker's version, or
  another worker did); when a row refers to what is neither held nor
  sent. A row held as it is is skipped. Refused, the frame is read by
  content from the worker's whole table, as before. A disk frame takes
  its rows in at every read, so the save's compaction cannot strand it.
- **The log looks back.** `TransactionLogCore::syncStrings` writes the
  rows past the highest id it stored; rows taken in under a lease are
  below what the GUI gave since. `takeRows` notes the lowest such id
  (`takeLowTaken()`), and the next sync goes back to it -- a version, and
  a worker reading it, would otherwise miss them.
- **Where it falls back, by design.** Two workers that make the same new
  string give it two ids; the first frame read in brings its id, and every
  frame of the other worker naming that string is then read by content.
  For the Animation that is the frames whose topology the version never
  had (a hole gone blind, a face split), made by more than one worker: the
  frames of one pass are the same object at other poses, so their new
  strings are the same strings. A recompute job is less exposed -- an
  object is recomputed by one job, and its names carry its own tag -- but
  two jobs over objects that share an input (a link, a common tool) can
  still meet. Not built: taking in what does not alias and rewriting only
  the names that name an aliased id, which `RestoreStrings` could do with
  a map of those ids alone.
- **Counted**: `cacheInfo()` `StringsById` / `StringsByContent` (reads of
  a worker's frame each way); the pool's `info()` `Leases` and
  `LeasesAsked`.

Measured 2026-11-16 on mini, the probe as above, after: a worker's frame
reads back in the time a frame made here does -- every frame by id.

| chain | worker frame, by content | worker frame, by id | own frame |
|---|---|---|---|
| 20 cuts | 20.4 ms | 11.3 ms | 12.3 ms |
| 60 cuts | 272.8 ms | 106.6 ms | 115.7 ms |

These frames change no topology, so a worker makes no new strings: the
rows it sends are all held already, and taking them in is the check
alone. Frames that do make new strings are the test below.

Tests: Toponaming_tests_run `StringHasherTest` 57 (+5: a reserved block
never given here nor after a save; a lease handing out its ids and asking
for more, a throw when there is none; a worker's strings back under their
ids; all or none on a clash, an orphan, the wrong order, an alias;
rows round trip as bytes), `TransactionLogTest` 107 (+1: rows taken in
below what the store holds reach it at the next commit -- fails with the
look-back taken out), TestAnimation 74 (+1: one worker, the plate grown
past the tools so the holes go blind, leases of 16 so it asks mid-frame;
every frame read by id, the same names when made here, and after a save
and a reopen), TestWorkerPool 23, Document 361, FileBlobs 97, TestPartApp
444, ctest 1268; GUI animation-workers 24, animation-cache 27,
animation-timeline 50, serve-animation 62, animation-export-clock 12,
serve-log 27.

## 16. Phase 5b, as built: what no view draws is held back

(2026-11-17.) Measured first, on the chain of sec 15 (60 cuts of a plate,
each hidden but the last, a fillet left out; 40 frames by 2 workers, read
back in the GUI, the main thread sampled): a frame read from disk took
2024 ms to show, 1875 of them in `evaluateAt`, and most of that a
recompute here -- the cache had dropped the frames.

- **The view built what it did not show.** Part's `SoFCCoordinate3`
  stands before the display mode switch and builds the visual when a
  bounding box traversal passes it, whatever the switch shows; the view
  asks the scene's box at every redraw. So every hidden object whose
  shape changed was meshed and filled at the next redraw: each hidden
  feature of a chain at every recompute, here or not (sec 14 had seen it
  as "a hidden object's visual is built all the same", and given the
  workers the meshes to match). Now `hiddenHere()`: not built when the
  traversal came through the object's own root and its switch shows
  nothing -- not for a link (a root of its own, and `forceUpdate()`), a
  switch override above, a view showing the object on its own. Built when
  shown, as `onChanged(Visibility)` already did.
- **So the workers mesh what is shown only.** The display hint is empty
  for an object no view builds. A frame of the chain was 150 MB with the
  meshes of 60 hidden cuts and 4.1 MB without any; it is 6.2 MB. At 150 MB
  forty frames passed the cache's disk bound (`CacheDiskLimitMB`, 4096):
  the first frame shown dropped twelve, and they were recomputed here.
- **What no view draws is held back.** `Animation::showAt(t)` is Time set
  for a picture: the player's tick, the timeline's scrub, an export of
  pictures (`videoexport`). A kept frame then puts back, of the geometry
  it holds (`PropertyComplexGeoData`), only that of objects a view draws
  (`Animation::drawnHere`, set by PartGui by the rule above; unset, as
  with no GUI, all are drawn). The rest is held back (`Owed`): a copy of
  the frame's value, or the bytes read from its file, unparsed -- a frame
  with any held back stays on disk only. Numbers, placements, anything
  else are put back as before.
- **Put in when something may read it** (`settle()`): Pause, the end of
  a scrub, a selection while playing (the player watches it), Time set
  (the frame put back whole), a recompute (`signalBeforeRecompute`, and
  before a frame made here), an edit or an undo (`signalBeforeChangeObject`,
  any write not the Animation's own), a save. Stop and a deactivation
  touch what is held back instead, so the recompute from the rest values
  makes it, rather than putting it in to be replaced. An object shown
  since is put back at the next frame. A read from Python while playing
  sees the shape of the last frame put in -- the lag D5 ruled.
- **Counted**: `cacheInfo()` `Deferred` (values held back, ever),
  `Settled` (put in since, ever), `Owed` (held back now).
- **Found on the way**: a change of an object's Visibility dropped the
  frames reaching it, as any change of it does, though no recompute of a
  chain reads it. Put right in sec 17.

Measured 2026-11-17 on mini, per frame shown, the view waited for
(`~/works/sw/fcad-probes/worker/profile-chain.py`, `profile-frames.py`;
`PROF_SHOW=1` steps with `showAt`):

| scene | frame | before | hidden not built | and held back |
|---|---|---|---|---|
| chain, 60 cuts | from disk | 2024 ms | 241-366 ms | 75.7 ms |
| chain, 60 cuts | from memory | 1170 ms | 167 ms | 68.1 ms |
| plate, 64 holes, filleted | from disk | 74-77 ms (sec 14) | 64.7 ms | 55.4 ms |
| plate, 64 holes, filleted | from memory | 56 ms | 52.4 ms | 52.9 ms |

The plate has one hidden object, so there is little to hold back; what is
left of its frame is the drawing (sec 14). Of the chain's 75.7 ms, the
replay is 38 ms: the shown cut's shape read and put in.

Opening a document paid the same (measured 2026-11-17 on mini, a plate
cut by a chain of 40 holes, the last cut shown, saved and opened again in
the GUI, the guard taken out by a temporary switch for the before). The
load paths leave a hidden object alone (`preMeshUndeferredLoad`,
`finishRestoring`; the progressive drain only where the object was hidden
when it was parked -- not always so, sec 18), and the first redraw's bounds
built all 80 hidden visuals anyway: 80 on-demand builds, 35 shapes meshed
from nothing. With the guard none is built, and the CPU from the open to
the view settled is 305 ms against 1071 (progressive load on; 338 against
1092 off). The open itself is the same, 170-240 ms. A hidden cut shown
later is built then, 80-110 ms here. (A hidden object's coordinate node
holds Coin's one default point, not a visual: what to look for to tell.)

Tests: TestAnimation 75 (+1: with no GUI, showAt() holds nothing back),
`tests/gui/animation-shown-holds-back.py` 24 (new: a hidden cut not built
when the chain changes, built when shown; a frame shown holds back the
plate and the hidden cuts, its picture the frame's; settle(), Time set, a
recompute, a selection, Pause, an edit, Stop each put them in, each the
frame's; a frame from disk read no further and staying there; a cut shown
since put back), TestWorkerPool 23; GUI animation-workers 24,
animation-cache 27, animation-timeline 50, animation-export 43,
animation-export-clock 12, serve-animation 62, serve-log 27, and 43 tests
of hidden objects, links, views of their own, loading and bounds: all
pass but five that fail the same with the guard taken out
(edit-on-top-by-preview-and-path, pd-edit-shown-per-view,
pd-preview-per-view, per-view-undo, selection-on-top-hidden-edges) and
drain-clock-step, which needs its clock shim.

## 17. A show or a hide is not an edit of the design

(2026-11-17.) Found in sec 16: an Animation dropped every kept frame
reaching an object whose Visibility changed, and the worker pool every job
reaching it, as for any change of the object. Showing the hidden cut of a
chain dropped the frames of every cut after it. The same went for any
property of a view provider -- a line width, a display mode: each touches
the object's `ViewObject` mirror (`ViewProviderDocumentObject::onChanged`),
which is a change of the object as far as `signalChangedObject` goes.

A colour is not one of them, and still drops: it is the object's own look
(`ShapeColor` over `ElementAppearance`, ShapeAppearanceDesign.md sec
14.6), which what is made from it takes when its shape is set (sec 14.6.2,
`MapFaceColor`). A kept frame holds the later cuts' looks as made from the
old colour; putting it back would put those back. Measured in the GUI test
below: a colour of the third cut dropped the kept frame holding the cuts
after it, through the write to `ElementAppearance`.

What a recompute reads of what the view shows is little, and known:

- **A group gathering its shown children.** Part's shape of an object that
  keeps none of its own -- an `App::Part`, a plain group, a link to one --
  is the compound of its children, the hidden ones left out
  (`Part::Feature::getTopoShape`). A child's Visibility reaches its group
  as a touch of the group's `_GroupTouched` (and `HiddenChildren`), a
  nested group's relayed upward (`GroupExtension::slotChildChanged`). A
  group with a shape of its own, a Body (its tip's), does not read it.
- **An expression naming it**: `Second.Visibility ? 1 : 2`, or a view
  property bound through `Obj.ViewObject.X`.
- **A Python feature**, whose code may read anything: for Visibility, the
  object itself or one linking to it is taken to read it.

`App::writeReaches(obj, prop)` (`WriteScope.h`) is the answer both now
ask. For a property of the design it is what it was: `obj` and every
object depending on it, `getInListEx(true)`. For Visibility, TreeRank, the
`ViewObject` mirror and the two relays it is the readers above and what
depends on them -- empty for a step of a chain or a feature of a Body.
The Animation drops the frames whose recompute met that
(`dropFramesReaching(obj, prop)`) and stops nothing when it is empty; the
pool still marks its copy stale -- the file it is a version of did
change -- but drops no job when nothing reads the change.

Not covered: a C++ feature that reads another object's Visibility in its
`execute()` and is none of the above; its frames would be kept past a show
or a hide. None found (2026-11-17, every `Visibility.getValue()` and
`isElementVisible` under `src/App` and `src/Mod/*/App`): the rest keep the
view in step (PartDesign's one-shown-feature rule, a solid's shown
target), run on demand (Assembly's interference check and frozen shape, a
random colour), or gather through a group, which the relay covers (an
Assembly's parts group; Body's visibility-gated shape is compiled out).

Tests: TestAnimation 78 (+3: a step of a chain shown and hidden keeps
every frame, put back and not made again; an expression reading a
Visibility drops the frames it reaches, made again with the value it reads
now; a child of an `App::Part` hidden drops the cut taking the group, made
again with a hole fewer; a Body's feature shown or hidden keeps every
frame; a step hidden while workers make frames drops no job and no
frame), TestWorkerPool 23, C++ WriteScope/Animation/Worker 5;
`tests/gui/animation-shown-holds-back.py` 26 (+2: a show and a line width
keep every frame; a colour drops the frame holding what follows), GUI
animation-cache 27, animation-workers 24, animation-timeline 50,
animation-export 43, animation-export-clock 12, serve-animation 62,
serve-log 27.

## 18. A hidden object a link does not show is not meshed at an open

(2026-11-17.) The guard of sec 16 changed nothing on the reporter's
`scanner.FCStd` (686 objects, 53 Links, 143 hidden Part features, saved by
a 2025 build at schema 4): opened in the GUI, 138 of the hidden features
had a visual built, with the guard and without it, and the open took the
same time both ways. No bounds walk built any of them. Two other paths did.

- **A Link forced every child.** A Link to an `App::Part` makes a
  `LinkInfo` for each object it shows, down the tree: the Part, its Body,
  every feature the Body claims (`LinkInfo::updateChildren`). Each one
  called `forceUpdate(true)` on its view provider as it was made, and a
  forced view provider is built whatever its Visibility says (`drawn()`,
  sec 16). A child is shown in the link through its child snapshot, whose
  switch follows the child's own Visibility (`LinkInfo::updateSwitch`): a
  hidden one was meshed for a snapshot that draws nothing. 125 of the 138.
  Now a `LinkInfo` forces its object only while it has a snapshot that
  shows the object hidden or not -- the transform and visible snapshots,
  i.e. as the object a link points at (`LinkInfo::updateForced`, from
  `getSnapshot`). A visible child is built anyway; a hidden one shown later
  is built then, as any hidden object is. The force is let go where it
  was, and before a delete too (`extensionBeforeDelete`), undo's reattach
  forcing again what still needs it.
- **The progressive drain built what was hidden by its turn.** A view
  provider whose record names a file of the archive -- a colour list, in
  a file written at schema 4 -- is restored at once rather than in the
  deferred view-provider drain (`restoreCapturedViewProvider`). Making it
  (`slotNewObject` -> `updateView`) asks for its visual before its own
  properties are read: its Visibility is still the default, shown, the
  shape's fault-in lands, and the visual is parked for the drain. The
  record then hides it; the drain built it anyway. With the Link fixed
  alone, that path still built 133 of scanner's hidden features. The drain
  and its pre-mesh submission now leave one no view draws touched and
  unbuilt (`runDeferredVisualSlice`), which is what an eager load leaves.

Measured on mini, `scanner.FCStd` opened in the GUI, CPU from the open
call to the view settled (two 1 s windows under 30 ms), 2 runs each:

| | progressive load (default) | eager load | migration recompute after the open |
|---|---|---|---|
| before | 13.0-13.3 s, 7.3 s wall, 138 hidden built | 9.8 s, 6.2 s wall, 125 built | 36.5-37.1 s, 15.8-16.0 s wall |
| Link fix alone | 13.2-13.7 s, 138 built | -- | 36.1-36.2 s |
| both | 11.5-11.8 s, 7.3-7.4 s wall, 5 built | 8.6 s, 5.9 s wall, 5 built | 34.5-34.9 s, 15.5 s wall |

The five still built are the ones Links point at directly, hidden in
their own place and shown through the link. The open call itself is 5.0 s
CPU (3.1 s wall) in every case with the progressive load. Pictures of the
settled view, before against after: the same objects, the differences a
sub-pixel shift of the fit that two runs of the same build show as much
of. The migration recompute (468 objects) is 2 s CPU cheaper; its wall
time is the recompute's.

Tests: `tests/gui/link-hidden-child-unbuilt.py` 33 (new: a Link to a
Part holding a Body with a hidden feature, a hidden plain child, and a
Link to a hidden object; saved at the current schema and at schema 4,
each reopened progressive and eager: the hidden children neither forced
nor meshed, the link's direct target forced and meshed, the link drawing
the Part, a hidden feature shown later built then. The tree before fails
16, with the Link fix alone 2). GUI tests of links, loading, drains,
hidden objects and views of their own, 36: all pass but
pd-edit-shown-per-view and pd-preview-per-view, which abort as they did
before (sec 16).

## 19. Phase 6, as built: a recompute made by a worker

(2026-11-17.) The second client of the pool (D6): `App::WorkerRecompute`
hands a document's recompute to a worker and puts the result in when it
comes, while this process carries on.

- **Measured first** (console, the reporter's `scanner.FCStd`, 686
  objects): its migration recompute executes 468 objects in 10.3 s, and
  the objects it reaches (527) are one group by their dependencies -- the
  largest group is the whole recompute. Splitting a recompute over
  workers would gain nothing on it; a job is one recompute. What it gains
  is the window: the GUI recompute takes 15.5 s, frozen.
- **One hook, both sides** (`App/RecomputeHook.h`). While a hook is
  installed for a document on this thread, `Document::recompute()` asks it
  before each object it would execute, and tells it after each one it did
  -- in the recompute loop and in `recomputeFeature()`, where an execute()
  recomputes another object itself (a split its pieces). Everything else is
  the recompute's own: the order, the signals, the touched state, the
  error log, the log's record.
  - In the worker (kind `recompute`), a recorder notes, for each object
    executed, what its execute() wrote (`signalChangedObject`, each value
    once, with the first writer; nested executes are entries of their
    own), whether it came out and its error, and how it left the other
    objects it wrote: touched or clean. The values are captured as the
    recompute leaves them, meshed as the view of the asking process meshes
    them (sec 14), with the strings they name under the leased ids (sec
    15) and the worker's table beside them for a read by content.
  - Here, a supplier makes each object from those values instead of
    executing it: it puts them in (`restoreValue`, a batch per object),
    sets its error or clears it, and leaves the other objects it wrote as
    the worker's execute left them -- a value put in touches its owner
    where the write there may not have (a split writing its pieces'
    `Parent`), and an object touched so would be executed again in the
    loop's second pass: 58 of scanner's were, before this.
- **What a worker cannot hand over** -- answered unfit, the document left
  touched for a recompute here: an execute that makes or removes an
  object or removes a property; one that gives things new numbers (a
  sketch's geometry ids, `Document::nextGeoId`, counted by the hook --
  handed over since, sec 36, when they are the numbers this process would
  give). A
  property an execute adds is handed over: its type, group, doc and
  attributes go with the result, and it is added here as the execute did
  (`Helix._BaseShape1`). What no save writes is each process's own and
  is left out: a shape's cache (`Prop_NoPersist`), the document's
  bookkeeping under an upstream name (`_ObjectUUID`, `Property::Legacy`).
- **What is made here by its type** (`RecomputeHereTypes` of
  Preferences/Workers, prefixes, `TechDraw::` by default): an object
  whose result is not all in its properties. A TechDraw view keeps its
  geometry in members and finds its hidden lines in threads that end
  after its execute(); a worker's values would put in a view with nothing
  to draw. The worker skips them; the recompute here executes them.
- **A value put in is not checked again**: `PropertyPartShape::
  validateShape` skips under `RecomputeHook::PuttingIn`, as it does for an
  undo. The worker checked it, fixed it where FixShape says, and its
  `InvalidShape` comes with it. On scanner that check was 91% of putting
  the result in (4.1 s of 4.4).
- **The job and its edits.** `reads` is what the recompute reaches, so an
  edit of any of it, or of anything it depends on, drops the job (sec 13);
  an edit between the result's coming and its being put in drops the
  result the same way. A recompute run here meanwhile takes the result if
  it came (`Borrowed`), waits for the worker if it is making it, and takes
  a queued job back and does it itself. Nothing depends on it: a recompute
  the workers did not make leaves the document touched, as it was.
- **Python**: `App.workerPool(doc).recompute(objs=None)`, `recomputing()`,
  `waitRecompute(seconds=-1)`, `cancelRecompute(reason)`,
  `recomputeInfo()` (`Applied`, `Borrowed`, `Cancelled`, `Failed`,
  `Unfit`, `Supplied`, `ExecutedHere` with `ExecutedHereNames`,
  `MadeHere`, `Unused`, `ById` and `ByContentWhy`, `WorkerSeconds`,
  `ApplySeconds`, `Bytes`, `Reason`).
- **The GUI** (`Gui/WorkerRecomputeGui.*`): Std_Refresh and the migration
  prompt's Yes (one document) hand the recompute over when
  `RecomputeCommand` of Preferences/Workers is on -- **on by default**
  since 2026-11-18 (sec 20, the worker's results matching the GUI's),
  "Recompute in a worker process" in the Document preferences; it was
  off at first because the result comes later than the command returns,
  which no caller of a recompute expects yet. The status bar says when it is made and how
  long each side took; one the worker failed or could not make is run
  here.

Measured 2026-11-17 on mini, `scanner.FCStd`, its migration recompute
after the open, 2 runs each (GUI: the view settled first; a 20 ms timer
says how free the window is):

| | made here | made by a worker |
|---|---|---|
| console: put in / its time on this thread | 10.3 s / 10.3 s | 13.2 s / 0.6 s start + 0.28 s put in |
| GUI: longest stall of the window | 7.5 s | 1.5 s (start) |
| GUI: lost in stalls over 100 ms | 12.6-12.7 s | 4.6 s |
| GUI: CPU of this process to the result | 28.3-28.5 s | 6.8 s |
| GUI: result in / view settled | 12.8 s / 13.8-14.8 s | 16.5 s / 17.5 s |
| GUI: peak memory of this process | 2.2-2.3 GB | 1.8-1.9 GB, the worker 0.2 GB more |

The worker's recompute is 10.2-10.9 s; its result 14 MB with the view's
meshes. In the GUI, putting it in is 3.0 s, of which the 218 TechDraw
objects executed here are part. The 1.5 s of start() is the log's
snapshot (`Document::_snapshotToLog`): a schema 4 file's shapes are not in
the store yet, and each is written out as a blob there
(`PropertyPartShape::makeBlob`, BRep text). That is the log's (oplab's)
part, not changed here.

What came out, compared object by object (volume, area, faces, element
map size, state):

- Console, worker against here: the same, all 405 shapes and every
  state, but Binder032 (below) and two infinite planes whose volume is
  noise either way.
- GUI, worker against here: 15 differ. They are one cause, and it is not
  the worker's: **refine depends on the meshes the faces carry**. The
  same unrefined Mirrored gives 17 faces bare, 19 with the meshes it
  carries in the GUI, 23 meshed at 0.1 mm (`removeSplitter`, either
  process). `ModelRefine::FaceUniter::process` checks a merged face's box
  against its faces' with `BRepBndLib::AddOptimal`, whose
  `useTriangulation` is on: a face with a mesh is boxed by the mesh, the
  merged face exactly, the two differ by more than the tolerance, and the
  merge is skipped. A console process -- every worker -- recomputes
  scanner's Mirrored to 17, the GUI to 19, and everything after it
  follows. Fixed the next day (sec 20): refine, and every App-side box
  whose answer a mesh could move, read the geometry now, and a worker's
  refine is the GUI's.
- The App::Part and link groups, gathered from shown children, come out
  as the GUI's -- the version carries what is shown.
- Binder032 (a sub-shape binder of a plain feature) keeps one mapped name
  more from a worker (`Wire1`, 50 against 49), in some runs; its geometry
  is the same, and its inputs in the worker were checked equal to this
  process's before the recompute. Not seen since (2026-11-20, sec 31):
  three runs, every one of the 405 shapes the same from a worker as made
  here.
- In the GUI, the strings of the result went by content: `takeRows`
  refused, "string 131105 is held here as 772" -- the worker gave a new id
  to a string this process holds under an old one. In the console they
  go by id. Correct either way; the cause is the strings' (oplab's) part.
  Not seen since (2026-11-20, sec 31): in the GUI too they go by id.

Tests: TestWorkerPool 30 (+7 `TestWorkerRecompute`: a Part chain and a
Python feature made by a worker, nothing executed here, its element
names those made here; an error put in with its message, what follows it
left as a recompute here leaves it; an edit dropping the job; a
recompute here taking the worker's result; an execute that makes an
object unfit; a property an execute adds added here; nothing to
recompute), TestAnimation 78, Document 361, TestPartApp 444,
TestPartDesignApp 1073.

## 20. What a recompute made depended on what had been shown

Sec 19 found the GUI and a worker making scanner's Mirrored differently,
17 faces against 19, and nine features after it with it. The cause was
not the worker's: it was that a result depended on which shapes carried a
display mesh -- what had been shown -- and a worker, as any console
process, has none.

**Refine.** `ModelRefine::FaceUniter::process` (modelRefine.cpp) checks a
merged face's box against the boxes of the faces it replaces, to the face
tolerance (1e-7; a fix is tried up to 10 times that). Both came from
`BRepBndLib::AddOptimal` with `useTriangulation` on: a face carrying a
mesh was boxed by the mesh's nodes grown by its deflection plus its
tolerance (BRepBndLib.cxx, the face branch), the merged face, new and
unmeshed, by its geometry. The refused groups of Mirrored were off by
1e-4 to 4e-3 against a 1e-6 window. So a group with one meshed face was
not merged, whatever the mesh's fineness:

| Mirrored, unrefined 24 faces, `removeSplitter` | before | after |
|---|---|---|
| bare | 17 | 17 |
| meshed at 1, 0.1, 0.01 | 23 | 17 |
| every other face meshed | 22 | 17 |

Both boxes, and the wire sort's, now come from the geometry (`false`). A
console recompute gave this already: the fix makes the GUI give what the
console does, and changes nothing in the console.

Measured over 23 files (the tree's PartDesign, Sketcher, CAM and example
files, and scanner.FCStd), every Part and PartDesign object recomputed
with no shape meshed, every other one, and all, each meshed as a view
meshes it, right after its recompute: before, 11 of scanner's objects
came out otherwise with meshes (Mirrored, PolarPattern, two helices,
Pocket021/026/029/034, Boolean001, Chamfer, Fillet007, each 1 to 3
faces more); after, none of the 501 objects differs between the three.
The saved file holds yet another of the old outcomes (Mirrored 19 saved,
18 with everything meshed, 17 now): there was no one old result to keep,
so there is no switch back to it.

What it costs: a recompute of a document saved where a mesh refused a
merge now merges, and a reference to one of the faces merged is lost. A
Thickness on either side face of two stacked pads, made where the sides
stayed apart, says "Invalid face reference" once they merge -- the merged
face is named anew, as when Refine is switched on.

**The same elsewhere.** Every App-side box that reads a mesh by default
was classified by what it decides. Those whose answer can move with a
mesh now read the geometry:

- `TopoShape::getBoundBox` -- Python's `Shape.BoundBox`, which
  expressions and Python features read in a recompute (CAM's stock from
  the model, BIM, Draft).
- `Shape.optimalBoundingBox()`: `useTriangulation` defaults to False now;
  asked for, the mesh still counts.
- Surface Extend up to a face (the natural extension's length, so its
  surface), `makELoft`'s check that two profiles differ (equal boxes to
  `Precision::Confusion()`), a helix's profile centre where the profile
  is a solid's face, TechDraw's view centre (`ShapeUtils::findCentroid*`,
  which asked for the mesh), a boolean's automatic fuzzy value,
  FaceMakerCheese's outer-wire sort, a sketch-based feature's through-all
  length and a hole's thread clip length (a hole writes a depth from it).

Left as they are: boxes used only as a pre-filter before an exact answer,
tools sized "big enough" (sheet metal, splitting tools), boxes of shapes
made in the execute itself or of copies (which carry no mesh), the task
panel's proposals, and FEM's glyph scale.

Tests: `test_refine_does_not_depend_on_display_meshes` (failed before: 4
faces against 3) and `test_bound_boxes_do_not_depend_on_display_meshes`
in parttests/regression_tests.py.

**The GUI against a worker again** (sec 19's probe, one run each): 3 of
686 objects differ, from 15 -- the two infinite planes, by volume noise,
and Binder032 (below). Mirrored is 17 faces in both.

**A worker's names spelled as this table spells them.** A string table
spells a mapped name's index either apart from its text (`#19:3`,
upstream's) or whole; a table restored from a file infers the mode its
entries were written in (`inferIndexedNames`). A worker's copy, opened
from the log, took the new-document mode instead -- on -- where
scanner's table is off. Every name the worker made was then spelled
otherwise than a recompute here spells it, and each reference here to
what the worker made missed its mapped name and was searched for by
geometry when the result was put in: 74 references more than a
recompute here renames. The setup a worker is sent now carries the
mode (`indexed`), and the copy's tables take it. After: the references
renamed are a recompute here's, the result 9.09 MB from 9.24. Test:
`testTheCopySpellsNamesAsThisTable`.

**The setting** (2026-11-18, the user: "make it default gated by a
setting"): `RecomputeCommand` is on by default, "Recompute in a worker
process" in the Document preferences. GUI test refresh-by-worker.

**The values stand in for the execute alone.** The hook made an object in
place of its whole recompute, from `Document::recompute()`, so what a class
does around its execute() was skipped at a put-in: a Part feature holds
its references while it executes and then updates them and reconciles the
shape generations kept for its referrers -- what a later recovery of a
reference searches. `DocumentObject::recompute()` now asks the hook in
place of `execute()` alone; the rest is the recompute's own. A class that
retries its execute within one recompute (a Part feature on copies of its
inputs) gets the same answer; a worker's entry for an object made here by
its type is not sent; an entry that carries nothing is not counted unused
(a link's element: its recompute in the worker came to no execute, here
it does, and there is nothing to put in either way).

**A feature's references in an order of their own.**
`PropertyLinkBase::updateElementReferences()` took them from an unordered
set of pointers. Binder032 is referred to by `Pad050.Profile` (its wire)
and `Pocket039.Profile` (its edges); which went first decided whether the
wire's name, derived from its edges', still matched -- one worker run in
three named the wire. Ordered by owner object id, then property name:
every run is now the same.

Open:

- Binder032: **fixed 2026-11-19** (docs/StringTableContentIds.md sec 14):
  the put-in emptied the document's string table -- a value captured after
  a Mutated binder's copy carried a table that its restore read into the
  document's -- and the geometry search then named the wire. 49 both ways
  now, and every object's names the same as a recompute here. What was
  seen before:
- Binder032: every run the same now, but a worker's 50 against this
  process's 49. Traced (`_updateElementReference` for `Pad050.Profile`):
  here the old name `#1efaa;Wire#1efa8;...` still resolves to Wire1; after
  the put-in it reads as missing, and the geometry search names the wire.
  The decoded names of the binder are the same in both: the worker spells
  the wire's derived name with a new id for the edge name
  `Edge989;:Hb42,E`, which this process holds as `#1efaa` -- its copy of
  that string does not match it by content. The table's and the log's
  copy of it (oplab's). Geometry the same; the reference resolves either
  way.
- In the GUI the result's strings still go by content: "string 131105 is
  held here as 772", 772 being `;SKT;:H4f6,E`, the postfix of one
  sketch's edge names. **Fixed 2026-11-18** (docs/StringTableContentIds.md
  sec 13): the worker's copy held 151 strings of the 41,048 here, because
  the 3D view had put its render properties' documentation in the
  document's table before the open read the file, and the open then kept
  the document's table apart from the file's, which the log stores. The
  marks (`Count` 0) were not it. In by id now, the put-in 2.34 s against
  3.01 s.
- **Worked on 2026-11-19** (sec 21): the open's version is the copy when
  nothing changed since, and the GUI now has one. What was seen before:
- Each snapshot of an unchanged document costs 0.35-0.5 s in the console
  (0.84 s in the GUI at start()), right after the open took one: every
  shape's sharing analysis (`ShapeRefSet::build`, 175 ms in the GUI
  sample) and congruence search (291 ms) run again whether the shape
  changed or not. The pool cannot use the open's version instead: after
  the open of scanner.FCStd the log holds version 1 at row 0 and its head
  is row 1 -- one row written after the open's version. A worker that
  opened version 1 and moved along that row would do; what the row is,
  and whether the open could take its version after it, is the log's.

## 21. The workers' copy without a snapshot (2026-11-19)

`start()` took a snapshot of the document for the workers' copy every
time: 0.43 s on the console for scanner.FCStd, 1.49 s on the main thread
in the GUI, where it was most of what a Std_Refresh by workers cost before
the window was free again.

- **A version nothing changed since is the copy.** `takeCopy()` first
  looks for the newest version of the document's branch with only records
  after it on the branch (`unchangedVersion()`, by `unchangedSince()`; a
  version at the root, seq 0, by its rows having no ops). After an open,
  that is the open's version; after a save, the save's. Preferences/Workers
  `ReuseVersion` (true) turns it off. Console, scanner.FCStd: `start()`
  0.07 s against 0.43 s, touched or not (a touch writes no row).
- **The GUI had no version of the open.** An older file's open takes its
  version in `afterRestore()` (sec 27.62 of the log); in the GUI, the view
  providers' and the views' first writes of the open (a page's `X`/`Y`, a
  view's `Size`, a removed `OnTopObjects`) had opened an implicit
  transaction by then, and a snapshot is refused while one is open. So the
  log held no value current, and every snapshot after serialised every
  shape. The version is now put off until that transaction is committed
  (`_commitTransaction()`), and taken then: the document as the open left
  it, the undo floor after it.
- **In the GUI the version is still not the copy** -- the GUI goes on
  writing after the open (sketches' `SavedNames` and `ProjectedGeometry`,
  views' properties), rows that change the document -- but the snapshot
  `start()` takes now finds every unchanged value current and writes only
  the rest: `start()` 0.55 s against 1.49 s on scanner.FCStd, the longest
  stall of a recompute by workers 720 ms against 1,488 ms. The results the
  same: 686 objects, only the infinite ones' volumes and the tree's
  expanded state differing, as before.
- **Not for a file of the current schema**: its open records the file as
  it is (`onRestore`: Document.xml as read and the blobs), not a value per
  property, and the snapshot at `start()` still writes every shape -- 1.35 s
  for the copy of scanner.FCStd that spells digests, saved by this build.
  Making that version one of values is the log's design (sec 23.3); left.
  Not wanted (the user, 2026-11-19: "No need to snapshot on open"): the
  write is once per open, at the first snapshot after an edit; sec 22 is
  what each snapshot after it costs.

Test: `TestWorkerPool.testTheCopyIsAVersionNothingChangedSince` -- after a
save, `start()` adds no version and the worker reads the document; after
an edit it adds one, and the worker reads the edit.

## 22. A snapshot costs what changed, not the document (2026-11-19)

Measured on the console, scanner.FCStd's copy of the current schema:
after the one write of every shape (sec 21), each `start()` after moving
one object still took 0.66 s. A sample of a loop of one-move snapshots put
72% of it in the congruence search of the shapes' files
(`CongruenceIndex::find`, `docs/SharedShapeStorage.md` sec 12.12): the
area and the volume of shapes, integrated again at every snapshot.

- **The shapes compared against.** A property keeps its own shape's
  invariants from one save to the next (`docs/TransactionLogJournal.md`
  sec 38.11), but the save's index held its stored shapes with invariants
  of their own, new at each save. So every candidate a later shape was
  compared against -- the same key, so the same vertices, but no match --
  was integrated again. On scanner most shapes are such candidates. The
  index's entry now shares the property's kept invariants
  (`CongruenceIndex::add(shape, slot, kept)`), and what a later shape's
  find computes of it is kept for the next save. A shape that changes
  takes new ones, never a reset: the index of a save in progress may still
  hold the old.
- **The key.** `shapeCongruenceKey()` walked every vertex of every shape
  twice per save, in `find()` and in `add()`. No rigid motion changes it
  either, so it is kept with the invariants.

| `start()` after moving one object, scanner.FCStd (schema 5 copy) | before | after |
|---|---|---|
| console | 0.65 s | 0.215 s |
| GUI | 0.72 s | 0.31 s |

The files written are the same: a save after warm snapshots and one from a
cold open give the same objects and the same blobs.

What is left, about 55% of the 0.215 s, is `ShapeRefSet::build` -- each
shape's sharing plan made again. Most of it is our OCCT fork's
`GeomTools_Curve2dSet::Add`, which prints every 2D curve to text to merge
equal ones. Not changed.

Test: `ShapeStorage.ShapeCongruenceCases.testASaveOfSavedShapesComputesNoInvariants`
(Python `Part.congruenceWork()`, the counts of keys and invariants
computed in the process): a second save of unchanged shapes computes none
(before: 3 keys, 1 length, 1 area, 1 volume), an edit one key.

## 23. A worker decides expression permissions as this process does (2026-11-19)

Sec 2 said a worker has the GUI's trust: the same expression-security
grants and policy. Phase 1 left it out. A worker read its own
`grants.json` (the same user directory, so the same file) once, at its
first check, and decided by that. What it did not have:

- the `--grant` overrides and the `--policy` file of this process;
- the session answers of the Document Permissions panel, which no file
  holds;
- a grant given or taken back after its first check. A grant taken back
  here still let the worker evaluate what this process now refuses.

And a check refused in a worker went nowhere: the expression failed, the
object's error was put in here as a recompute's result, and the user was
never asked -- the pending request was the worker's.

**The state goes with the work.** `Runtime::exportState()` is what decides
here: whether enforcement is on, the overrides, the session answers and the
store (`GrantStore::toText()`). The setup a worker is sent carries it, and
a job carries it again whenever it has changed since the worker was last
told -- a grant or a revocation reaches a worker before the next job it
runs. The worker adopts it (`Runtime::adoptState()`) and decides by it
alone: it never reads or writes `grants.json` from then on. Once answers
are not carried: they are for the re-run here they were given for.

**A refusal is answered here.** A check that refuses in a worker -- a deny
or a prompt -- counts (`Runtime::refusals()`), and a job during which one
did answers unfit: a recompute leaves the object to be made here, a frame
is made here. Here the check is made again, recorded in the audit log and,
where it may be granted, asked for. The worker writes no audit line and no
pending request for it.

Test: `TestWorkerRecompute.testAWorkerDecidesPermissionsAsHere`: an
expression reading `_app` (app.query, which a document must be granted)
under a session grant here is made by the worker; with the grant taken
back, the worker's job is unfit, the object is made here, refused, and the
request is pending here. Without the state sent, the first part fails: the
worker refuses what this process granted.

## 24. An export made by a worker (2026-11-19)

The user, 2026-11-19: "work on export", the looks being the objects' now
(docs/ShapeAppearanceDesign.md sec 14.6.7: the exporters read what an
object draws through the object, with no view provider). Std_Export of a
STEP or an IGES file -- ImportGui's in the GUI -- is written by a worker
from its copy of the design, and the window is free meanwhile.

**The kind.** `export`, in `freecad/worker.py`: `Module.export(objs, file
[, options], **keywords)` on the worker's copy, the call an export here
makes. Its inputs name the module, the objects (in order), the file, the
options and keywords, and a file of this process's Preferences as they
were when it was asked for: a worker's own settings are from when it was
started, and the exporters read theirs there -- the STEP header's author
and company, the scheme, the units. The STEP and IGES writers' settings are
OCCT's process globals, which Part sets from the preferences once, when it
loads: `Part.applyExchangeSettings()` sets them again
(`OCAF::ImportExportSettings::initialize()`). An object the copy has not
is unfit.

**Pinned to the design as asked for.** An export is of the design when the
user asked, not when a worker gets to it. `Job::pinned`: the job is made on
the copy standing when it was submitted. No change of the design drops it
(`reads` is not looked at); when a newer copy is taken, a pinned job of the
old one is handed, before the word to move, to a worker on that copy with
nothing to do (a socket keeps order), its copy's directory and log version
held until it is done (`heldCopies`, released by `pruneCopies()`). A pinned
job no worker on its copy can take then -- every one busy, or none
connected yet -- comes back Cancelled ("the design changed before a worker
could make it"). A worker that dies over a pinned job tries it again only
while its copy is the one standing.

**The client** (`Gui/WorkerExportGui`, from `Gui::Application::exportTo`):

- Only a module in `ExportModules` of Preferences/Workers
  ("Import;ImportGui;Part;Mesh"), and of ImportGui only STEP and IGES: they
  are Import's (`ExportOCAF2::objectColors()`/`objectAppearance()`, the
  looks through the object). ImportGui's glTF reads render settings only
  view providers have (`Render_*`), and stays here; so does a module that
  chooses its own objects (`exportSelection`). `ExportCommand` of
  Preferences/Workers, on unless set off: "Export in a worker process" in
  the Document preferences.
- The options are asked for here, where the module's dialog is (ImportGui's
  STEP dialog), and go with the job as Import's keyword arguments
  (`exportHidden`, `keepPlacement`). The legacy exporter, an option Import
  has no keyword for, or options that are not JSON: exported here, with
  the options asked for, and not asked again.
- The macro records the export as if it had run here.
- Done: the status bar says so, with the worker's seconds, and the file is
  added to the recent ones as an export here adds it. The exporter failed:
  "Export failed" with its message, as here. The worker died over it: told,
  and not tried here, where whatever ended the worker would end the
  session. Unfit or cancelled: exported here if the document did not change
  since it was asked for; otherwise told, to export again.

**The worker's copy is named as the document.** A STEP file's root is named
by the document's name (`ExportOCAF2`, several objects), and a worker named
its copy `Copy`. The setup now carries the document's name and label; the
copy takes them, and a copy saved for the workers is saved under the name.

**What a worker writes is what the document reopened writes.** Compared
with an export of the live document here, a worker's file differs where an
export of the same document saved and opened again here differs too, and
nowhere else:

- a colour that is not on a 1/255 step -- the default material's diffuse
  0.9 -- is written from the float here and from the 8 bits a document file
  keeps of it after an open (0.90196104 against 0.90196080);
- the shapes read back share their equal geometry, and the writer meets it
  in another order: two directions trade a zero's sign.

And two files written by two processes differ anyway: OCCT numbers the
assembly usages by a count of its process, and writes the colours' groups
(`STEPCAFControl_Writer`, `myMapCompMDGPR`, one per shape) in the order of
a map hashed by the shapes' addresses -- the same saved file, exported in
six processes here, came out in two orders. Neither is changed: the first
is the document's precision, the second OCCT's (our fork; not touched).

| scanner.FCStd (schema 5 copy), its 76 shown shapes to STEP (19.6 MB), console | this process held | file written after |
|---|---|---|
| here | 3.11 s | 3.11 s |
| by a worker, none running | 0.07 s | 4.79 s (the worker's start, then 3.10 s) |

Tests: `TestWorkerPool.testAPinnedJobIsMadeOnItsCopy` (an edit and a newer
copy while it runs: it is made on its copy, which stays until it is done),
`testAPinnedJobNoWorkerTookIsCancelledByANewerCopy`;
`TestWorkerExport`: a box with a painted face, a cylinder and a link
overriding its colour -- the worker's file has the geometry of the
reopened document's export byte for byte and the same colours read back;
an edit, a recompute and a newer copy after it was asked for leave the
file the design as asked; a preference set since the worker started (the
STEP company) is in its file; an object the copy has not is unfit. GUI test
`tests/gui/export-by-worker.py` (12 claims): Std_Export of a STEP file
returns with the export with a worker; the file comes, the box edited and
recomputed right after the command written at its old size, its faces'
colours right; set off, it is written here at once, with the colours the
worker wrote.

## 25. A geometry check made by a worker (2026-11-19)

Part_CheckGeometry ran BRepCheck_Analyzer and, when asked, OCCT's
BOPAlgo_ArgumentAnalyzer on the GUI thread, a progress dialog the only
thing left to the user. Measured on the console, scanner.FCStd's 76 shown
shapes: BRepCheck alone 3.4 s, with the BOP check 43.2 s, one shape
(`AxisBottom`, 35 faces) 14.2 s of it. The BOP analyzer is also where OCCT
fails hardest on bad geometry. The check now runs in a worker by default,
and the panel fills when it comes.

**One check, two places.** The panel's walk moved to Part App, unchanged in
what it finds: `Part::GeometryCheck::check(shape, options)`
(`Mod/Part/App/GeometryCheck.*`), Python `Part.checkGeometry(shape,
options)`. A finding names its shape by position -- its type and its index
among the checked shape's shapes of that type (`TopExp::MapShapes`), the
index the panel names an entry by (`ResultEntry::buildEntryName`) -- and
the finding it is under. The panel makes its entries from findings
(`putFindings`), on the shape it holds, whether they were made here or by
a worker; the options are the panel's settings, read here and sent. Two
things moved with it: the BOP check's entries are put on the checked
shape, not on the copy the analyzer is given (`BRepBuilderAPI_Copy` keeps
the order, so the names are the same), and each such entry refs the
separator it draws its box in (it ref'd its parent's).

**The kind.** `part.check` (`freecad/worker.py`): for each item -- an object
and a subname, `Part.getShape(obj, sub, needSubElement=True)` as the panel
takes it -- `Part.checkGeometry` with the options. Its `reads` are the
objects: an edit of one, or of what it is made from, drops it, and the
panel says to check again.

**The panel.** CheckCommand of Preferences/Workers ("Check geometry in a
worker process" in the Document preferences), on unless set off. The
shape content (`BasicShapes.ShapeContent`) is still made here. A worker
that failed or is unfit: checked here. One that died over it: told, and
not tried here. Closing the panel drops the job, and a worker that has
checked for long is ended (sec 26).

Tests: `TestWorkerPool.TestWorkerCheck`: a bow tie face (invalid, its wire
self-intersecting), two overlapping boxes in a compound (valid to BRepCheck,
faulty to the BOP check) and a box's face: the worker's findings are the
ones made here, with and without the BOP check; an edit of what it checks
drops it. GUI test `tests/gui/check-by-worker.py` (8 claims): the panel
opens with the check with a worker and says so; the results come, both
shapes invalid, the wire self-intersecting, the boxes faulty to the BOP
check; set off, the same rows, made here at once.

## 26. The worker of a long job that was dropped is ended (2026-11-19)

A job is dropped while a worker makes it when its client cancels it
(`cancel()`: the Check Geometry panel closed, a recompute cancelled) or an
edit reaches what it reads (sec 4). Until now its worker made it to the end
and the result was thrown away: a dropped BOP check of the scanner model
kept a worker for up to 43 s, and kept a later job waiting for it the whole
time when the pool has one worker.

**The rule.** A dropped job that has run longer than `EndAfter` has its
worker ended (killed: a worker in OCCT cannot be interrupted). A job that
has run less is left to finish. How long a job has run says little about
how long it has left, and a new worker costs a start, a copy opened whole
rather than moved along the log, and the imports its jobs need. Edits drop
short jobs all the time, and ending a worker for each would replace
workers over and over while someone types. `EndAfter` is the larger of
what a worker took from its start to its copy open, averaged over the
pool's workers (`StartSeconds`, which grows with the design), and
EndDroppedAfter of Preferences/Workers, default 2 s. The worker on the
Mac mini reaches ready in 0.14 s on a small design and 0.26 s in the GUI,
so on a small design the 2 s floor is the threshold.

An ended worker is not a failure: it does not count towards the three in a
row that stop the pool, a dropped job is not tried again, and the queue
goes on, on a new worker if none is left (`Private::endDropped`).
EndDropped (default true) set false leaves every worker to finish.
`info()` says `Ended` (workers ended so), `StartSeconds` and `EndAfter`.

The Check Geometry panel cancels its check in `closed()`, the moment it
closes: it used to do so in its destructor, which runs only when the
deleted panel is collected later.

Tests: `TestWorkerPool`: a minute's job cancelled after `EndAfter` ends
its worker, and the next job is made by a new one well within the minute;
a job cancelled as soon as it runs is finished by its worker, which makes
the next; an edit dropping a long job that reads what was edited hands it
back Cancelled, not Died, and ends its worker; EndDropped off leaves the
worker running. GUI test `tests/gui/check-by-worker.py` (10 claims now): a
check of 625 overlapping spheres, the panel closed after `EndAfter`, and
its worker is gone at once.

## 27. What a worker says is said here (2026-11-19)

A worker's warnings and errors went only to its log, in the pool's
directory, which is deleted with the pool: a Pad's "face is not planar"
made by a worker, or a STEP writer's complaint, was never seen.

**The relay.** A worker attaches a Console observer (`Relay`,
`WorkerPool.cpp`) once it has said hello: everything said through the
Console -- `FC_WARN`, `Base::Console().error`, `App.Console.PrintWarning`
-- goes to the pool as a `log` message, with its level, recipient, content type and notifier.
What the process prints to stdout still goes to the log as before. A log
line (`LogStyle::Log`) goes only when RelayLog of Preferences/Workers is
true (default false), sent with each setup; the rest always goes. Said on
another thread, a message waits under a lock for the next one said on the
worker's main thread, which owns the socket, or for the job's result or
ready, which flush it first: it arrives before the result it belongs to.

**Said with the result.** The pool keeps what came while a worker made a
job with the job (at most 500, then one line saying how many more are in
the log) and hands them over in `Result::messages`. After `done`, those of
a result Done or Failed -- including Failed because the worker died, when
what it said last may say why -- are said here through this process's
Console (`WorkerPool::show`), the notifier "worker N", or "Pad (worker
N)" for one that named one. A client may take them inside `done`: say
them itself, or clear the ones it says another way. Those of an Unfit
result are not said, since the job is done here and says its own, and a
dropped job's are thrown away with it. What comes while a worker has no
job -- opening a copy, moving along the log -- is said at once. Python
results carry them as `Messages`: dicts of Level, Notifier, Text, Worker.

**A recompute's failure, once.** A recompute made by a worker (sec 19)
failing at an object has the worker's Document say "Failed to recompute
X", and the put-in here says it again as it puts in that failure, as a
recompute here does. `WorkerRecompute`'s `done` drops the worker's.
What the object's `execute()` said there stays: it is not run here.

Tests: `TestWorkerPool`: a job's warning, message, error and a warning
said from another thread come with its result in order and are said here
as "worker N", a log line not unless asked for; with RelayLog on, it is,
with what the worker logged opening its copy; an Unfit job's are in its
result and not said; a dropped job's are not said;
`TestWorkerRecompute.testAFailureTheWorkerSawIsSaidOnce`. GUI test
`tests/gui/messages-by-worker.py` (5 claims): nothing in the Report view
while the job runs; after it, "worker 0: careful there" and "worker 0:
that went wrong"; an Unfit job's words not shown.

## 28. An edit's long recompute made by a worker (2026-11-19)

The property editor recomputes as it commits an edit
(`PropertyEditor::closeTransaction`), inside the edit's transaction. That
is the recompute users wait on most: the window is held for as long as
whatever the edit reaches takes to execute. Nothing in the editor uses the
result once it is made.

**What a recompute will take.** `WorkerRecompute::trackTimes()`, started
with the GUI's Application, notes what each object's last recompute took,
by its id. In a recompute here, that is the gap between one object's
`signalRecomputedObject` and the one before it, or the recompute's start.
The gaps add up to the recompute's time, and an object an execute()
recomputes itself is counted on its own. When a worker's result is put in,
the time is the worker's execute()'s (`Supplier::executed`); an object
executed here meanwhile gets this process's time.
`WorkerRecompute::expectedSeconds(doc)` sums those times over what the
recompute would reach: what is touched, and everything depending on it
(`reachOf`, the same set `start()` hands over). An object not made since
the tracking began counts for nothing, and is counted in `unknown`.
Python: `pool.expectedRecompute()` -> (seconds, unknown).

**The editor.** When the edit's recompute is expected to take at least
RecomputeEditSeconds of Preferences/Workers (0.5 s by default) and
RecomputeEdits is on (by default; "Recompute long edits in a worker
process" in the Document preferences), the editor commits the edit first
and then hands the recompute over (`Gui::editRecomputeIsLong`,
`recomputeEditByWorkers`). A transaction left open would have the pool
take a saved copy of the document. If it cannot be handed over, it is
recomputed here, after the commit. A shorter one is made here at once,
inside the transaction, as before: a worker's would come later than that.
Undo is not changed by this. The result put in later goes into the
edit's step: one undo takes back the edit and its result together, and
one redo brings both back (both checked in the console; the GUI test
checks the undo).

Measured on mini, `scanner.FCStd`, an edit of Pad039's Length (22 objects
reached, 6.5 s expected), in the console:

| | made here | made by a worker |
|---|---|---|
| this thread held | 6.6 s | 1.7-2.0 s the first time (the pool starts), 0.45 s after |
| the result in | 6.6 s | 10.5-11 s |
| the worker's recompute / put in here | | 6.5-6.7 s / 0.15 s |

The worker's result came about 4 s after one made here would have. Most
of that was the move along the log: the job ran 2.1 s after it was handed
over to a worker already started. Sec 29 takes it out: 7.3 s now.

After an open, no object had a time, so an edit's first recompute was made
here, and that recompute gave the objects their times. Sec 30 reads them
from the file's recompute records instead.

Tests: `TestWorkerRecompute.testARecomputeIsExpectedToTakeWhatItsObjectsTookLast`
(objects never made are unknown; a 0.3 s execute and what depends on it,
expected 0.3-0.6 s; a quick one under 0.1 s; made by a worker with a 0.8 s
execute, expected 0.8-1.2 s). GUI test `tests/gui/edit-recompute-by-worker.py`
(10 claims), which types into the property editor:
- an Input for an object whose execute() took 1 s: the editor is back in
  0.04 s, the recompute is with a worker, and the result comes, made by
  the worker's process;
- one undo step for the edit, which takes back the edit and its result;
- an object that took no time is made here at once;
- with RecomputeEdits off, the slow one is made here, 1.02 s.

## 29. A worker keeps what it made as it moves across it (2026-11-19)

The move along the log that brings a worker to the next copy (sec 4)
crosses the rows its own last result was put in as. On scanner, after an
edit of Pad039's Length, the step held 7 values: the Length, and the
Shape and AddSubShape of the three objects the worker had just made. The
move restored all 7, and the three shapes took 3.5 s of it.

**Why each was restored.** A move puts in only a value that differs from
the document's: it writes the document's value again (`captureValue`)
and compares the bytes. A worker's own result never compares equal:
- the log's copy was written by the other process, after the worker's
  bytes were read there, under that process's naming: `HasherIndex="0"`,
  and the attachment named `Property.brp` rather than `Pad039.Shape.brp`,
  the shape's bytes themselves equal;
- one value (PolarPattern003's shape) differs in the last digits of a
  location's matrix (`-0.41084317105790380` against `...374`): a
  location read back is normalised, so what was read and written again
  is not what was made.
Bytes cannot show that it is the same value, and comparing shapes by
geometry would cost what restoring them does.

**What the pool tells the worker.** The process that puts a result in
knows which of its rows that was. `WorkerRecompute::apply` notes the log's
`lastSeq()` before and after the put-in, and the objects made from the
worker's values (`Supplier::fromWorker`; one executed here is not), and
tells the pool (`WorkerPool::putIn(job, from, to, objects)`). The pool
holds it for the worker that made the job (`Proc::own`), as long as that
worker has had no job and no setup since, and sends it with that
worker's next move (`own` in the setup).

**What the worker keeps.** The worker checks it too: its last job must be
that one, with no setup since. It moves with a `LogMoveKeep` installed
(`App/LogMoveKeep.h`, a hook for the scope of the move, like
`RecomputeHook`). `_moveAlongLog` notes, for each value a recompute wrote
going forward, the row that wrote it (`LogFold::writer`; any later op on
the same property clears it). It asks the hook of each such value before
it compares, and leaves the value as it is when the hook agrees. The
hook agrees for a value that is:
- written in the put-in's rows;
- of an object made from the worker's values;
- one the worker's last recompute sent (`WorkerRecompute::sent`).
An edit after the put-in is a later write of its own, not a recompute's,
and is put in as before: the Length was. What the worker holds of a shape
is what it sent, meshes included: the mesh for the view is made on the
worker's own shape (`captureMeshed`), not a copy. `ready` says how many
were kept; `info()["Kept"]` sums them.

Measured on mini, scanner, three edits of Pad039's Length in turn, each
recomputed by the worker:

| | before | after |
|---|---|---|
| the move: values restored / kept | 7 / 0, 3.6 s | 1 / 6, 0.2 ms |
| the result in, from the hand-over | 10.8-10.9 s | 7.3-7.4 s (6.6 s made here) |

After the three, the 405 shapes made so were compared with a recompute
here of the same state (volume, faces, element map size, state): none
differs.

Tests: `TestWorkerRecompute.testAWorkerKeepsWhatItMadeAsItMovesAcrossIt`:
a box cut by a rotated cylinder, and a counted feature reading the box.
The box's Length is edited and recomputed by the worker; then its Height
twice, each recomputed by the worker. Each result is right, at least two
moves keep values (`Kept` > 0), and the last one equals a recompute here,
element map included.

## 30. What a recompute took, read from the log after an open (2026-11-19)

A recompute here leaves a record in the transaction log: a row of kind
`recompute` whose script is JSON, with each object's id and the seconds
its execute() took (`TransactionLog::onRecompute`). A file with its
history carries those rows. `WorkerRecompute::expectedSeconds` reads them
once per document, at its first ask (`readRecorded`). It reads the newest
2000 rows of the chain to the head, and keeps for each object the most it
took in any of them: the record of a worker's result put in here holds its
putting in, a fraction of its making. A time measured since the tracking
began comes first. An object with neither is unknown, as before. So the
first edit after an open whose recompute was long when last made goes to
a worker too. A file without history -- schema 4, as scanner is -- has no
records; its first recompute here gives the times.

Test: `TestWorkerRecompute.testAnOpenKnowsWhatARecomputeTookFromTheLog`.
A 0.4 s execute and what depends on it are recomputed, saved and opened
again. An edit with nothing recomputed since is expected to take
0.4-0.8 s, nothing unknown.

## 31. The recompute at the end of an edit, and the open items checked (2026-11-20)

**A PartDesign dialog's OK, a sketch's edit left.** Both end in a
recompute of the document: `TaskDlgFeatureParameters::accept` checks the
feature it made and keeps the dialog open if it failed, and
`ViewProviderSketch::unsetEdit` recomputes what the sketch changed. What
follows the edited object -- the rest of the body, what reads it -- is
the part that can be long. The edited object itself is not a worker's to
make. The dialog has to know at once whether it came out. A sketch's
execute may give geometry new ids, which made a worker's job unfit (sec
19; taken in since, sec 36).

`Gui::makeEditedFirst(edited)` sums what uses the object
(`expectedSeconds` over its InList, sec 28). Below RecomputeEditSeconds
it does nothing, and the caller recomputes as before. At or above it, it
makes the object here (`Document::recomputeFeature`), clears its touch
once it comes out valid, marks what uses it to be recomputed, and says
so. The caller checks the object as it always did, closes its step, and
hands the rest over (`recomputeEditByWorkers`), or recomputes here if
that does not take it. The dialog's OK leaves the feature as made when
its preview made it (upstream c1c9cb63e0), so then only the rest is
made.

**What is marked here goes with the job.** An object marked to be
recomputed (`enforceRecompute`) has nothing in the log that says so: the
worker's copy holds it untouched and its recompute passed it over. The
sketch's Pad was not made again. `WorkerRecompute::start` sends, with
the objects, those of them that must be recomputed here (`touched`), and
the worker marks them before it recomputes.

What the estimate counts is everything that depends on the edited
object. A recompute skips one whose inputs did not change (a feature
reading `Pad.Length` after a sketch edit), so the estimate can be more
than what runs, and a recompute that turns out short is handed over
anyway.

**The open items of sec 19, checked again on scanner:**
- Binder032's extra mapped name is not seen. In three console runs, a
  worker's recompute and one made here at the same point give the same
  volume, faces, element map size and state for every one of the 405
  shapes, but the infinite datums, whose volume is noise.
- The strings of a worker's result go by id in the GUI too (`ById` True,
  nothing read by content), as they do in the console.
- Found on the way, not the workers': two recomputes here in turn of
  scanner just opened do not give the same element maps. Sketch046 has 18
  mapped names after the first and 16 after the second, Sketch047 16 and
  15, Hole004 301 and 300. A worker gives what a recompute here gives at
  the same point. Scanner is a schema 4 file, and the first recompute
  probably still carries names from the old element-map version.
  **Explained 2026-11-20, not a fault:** each name that goes is a wire's
  that another object refers to -- Sketch046's Wire1 and Wire2 (the
  profiles of Pad039 and Pad040), Sketch047's Wire2 (Pocket023), Hole004's
  Wire3 (Binder033), Binder032's Wire1. A wire is not named when its shape
  is made: `Part::Feature::getExportElementName()` makes its name from its
  edges' the first time something asks for it, and keeps it in the map.
  The first recompute after the open changes the element-map version, so
  every reference is looked up again (`GeoFeature::updateElementReference`
  with `reset`), which asks; the next recompute makes the shape again and
  nothing asks. Asked after it, each wire gives the very name the first
  pass kept, the objects using them are valid after both, and every
  "auto change element reference" is the first pass's (95), none the
  second's. A third recompute is the second. The same on the copy that
  spells digests, where it is these four objects on its first recompute.
- Refine and the display mesh: done in sec 20.

Tests: `TestWorkerRecompute.testWhatIsMarkedHereIsMadeThere`. GUI test
`tests/gui/edit-end-recompute-by-worker.py` (10 claims): a body with a
sketch and a Pad, and a Python feature reading the Pad's volume whose
execute() takes 1 s.
- A length typed into the Pad's dialog, then OK: the dialog closes in
  0.08 s, the Pad is made here, and what reads it comes from the worker.
- A sketch's distance changed, then its edit left: back in 0.1 s, the
  sketch made here, and the Pad and the feature by the worker.
- With nothing long after it, OK makes everything here at once.

## 32. A worker's ids stay above what it has read (2026-11-20)

Found while measuring sec 15 of docs/StringTableContentIds.md, and wrong
since sec 15 here: a file saved after a worker's second job could come
back with "Invalid string id reference" and its table cut short.

- **Why.** A worker is leased a range of ids once (sec 15), 16,384 of
  them, and uses it over all its jobs. This process goes on giving ids
  above that range. At its next job the worker reads those strings (a
  move), and a string it then builds on one of them gets the next id of
  its old range: below the id it refers to. A table is saved in id order
  and read row by row, each reference looked up as it is read
  (`restoreStreamNew`); `insertRows` and `merge` did the same. A fillet
  recomputed by a worker, a second filleted box made here, their cut made
  by the worker: 7 strings of the cut's names referred to a higher id, and
  the file reopened with the error, 61 of 103 strings read.
- **The range is let go.** `StringHasher::nextID()`: when the next id of
  the lease is not above every id the table holds, what is left of the
  range is dropped (`StringIDLease::drop()`, `stale`) and another asked
  for. The pool answers a request that says `stale` with a small range
  (1,024, or `IdBlock` when that is less; the doubling of sec 15 is for a
  job that ran out), cut from above everything it has given. So a new
  string's id is above every string it can be built on, in a worker as
  here. A worker whose table took in nothing higher keeps its range.
- **What holds a reference forward is still read and written.** For the
  files and stores written before this:
  - `restoreStreamNew` takes a row whose reference is not there yet once
    it is; "Invalid string id reference" is for one that never comes.
  - `insertRows`, `takeRows` and `merge` likewise: a row waits for the one
    it refers to when both come together.
  - `saveStream` writes what a string refers to ahead of it when that id is
    higher, and such rows with their id and references in full: the table
    is then one every build reads, older ones too.

The same three objects now: no string referring to a higher id, 2 leases
(the second the small one), the file reopened whole.

Tests: `StringHasherTest.aLeaseBelowAnIdTakenInSinceIsLetGo`,
`rowsReferringToOneFurtherOnComeIn`,
`aStringBuiltOnOneOfAHigherIdIsSavedAndReadBack`,
`aTableSavedInIdOrderWithAReferenceForwardIsRead` (a table as it was
written before, read); `TestWorkerRecompute.
testAWorkersLaterStringsAreSavedAndReadBack` (the fillet, the box made
here and the cut, saved and reopened: every id the cut's names spell is
in the table).

## 33. A script that ends with work on the thread pool (2026-11-20)

Seen while measuring sec 32, not the workers': `ThunderealCmd` died at
exit (signal 11 or 6) when a script ended while TechDraw was still
finding a drawing's hidden lines or faces. 8 of 10 runs of scanner
reopened by a script that then ended.

- **Why.** A view's projection, its faces, a section's cut and a detail
  run on Qt's shared thread pool, and the projection reads the parameters
  when it is done (`DrawUtil::isCrazy`). `Application::destruct()` let go
  of the parameters, the console and the interpreter, and the statics'
  destructors then freed OpenCASCADE's types, with that work still
  running: the crash was in `ParameterGrp::GetGroup()` on a pool thread,
  or in `Standard_Type`'s destructor. The GUI was not hit: Qt waits for
  the pool and deletes it as the `QCoreApplication` goes, which is before
  `destruct()`. The console has no `QCoreApplication`.
- **Fix.** `destruct()` first waits for what the pool runs (at most
  120 s, then a warning), when there is a pool left: in the GUI there is
  none, and asking for it gave null -- the first version called it and
  crashed every GUI exit. `App::isShuttingDown()` (App/ShuttingDown.h)
  is set first: TechDraw's tasks that are queued and not yet begun end at
  once instead of doing their work for no one. What runs cannot be
  stopped.
- **Measured**: the same 10 runs, all exit 0. The GUI tests of TechDraw
  and the workers exit cleanly. `td-stored-hatch` fails one or two of its
  claims with and without this, so that is not this change.

Test: `Document.UndoRedoCases.testAScriptEndingWithThreadPoolWorkWaitsForIt`,
a script that queues a task on the pool which ends after the script does;
the task finishes and the process exits 0. Without the wait the process
ended in 0.37 s and the task never finished. A TechDraw script did not make
the crash happen reliably enough to test with: a console recompute waits
for a view's hidden lines, and only its faces are left running after it.

## 34. An object that crashes a worker costs the worker (2026-11-20)

The user, 2026-11-20: "do 6 first" -- crash isolation as a feature.
Before, a worker that died in a recompute was tried once more, the job
then failed, and nothing said in which object: the objects stayed
touched, the next recompute here -- or every one, once three deaths had
stopped the pool -- ran the object that crashed in the application itself.

- **The worker says where it is.** `WorkerPool::Request::at`: the handler
  tells the pool what it is on, as it changes (a message `at`, written out
  before the next object begins). A recompute's recorder says each object
  as its execute() begins, and the one it is back in when an inner one is
  done. The pool keeps it with the job.
- **A death is put down to its place.** When the worker goes, the pool
  notes where the job was and how the process ended (`endedHow()`:
  "crashed (signal 11: Segmentation fault)", "exited with code 3", "ended
  by the pool"; the socket closing is given a second for the process to
  be reaped first, so a crash is not read as the pool's kill). A failed
  result carries both (`Result::diedAt`, `how`; Python `DiedAt`, `How`),
  and the error says them: "its worker died over it (crashed (signal 11:
  Segmentation fault)), at Crash". A death in an object's execute() is
  the object's: it no longer counts towards the three in a row that stop
  the pool.
- **The object is quarantined.** A recompute job whose workers each died
  in the same object's execute() (`quarantine()`): the object is left in
  an error -- "its recompute crashed a worker process twice (signal 11:
  Segmentation fault). It is not recomputed in this process, which it
  would crash too: change it, or what it uses, to have it tried again" --
  on the object and in the Report view. `Document::_recomputeFeature()`
  asks `RecomputeHook::refuses()` before an execute, and a quarantined
  object is left in that error, not executed: in a recompute here, in its
  own recompute, in a task panel's. What depends on it is not made from
  it. A job given to a worker afterwards says which objects are refused
  (`refused`), and the worker leaves them in the same error: no worker
  dies again over it. Deaths in different places, or with nothing said,
  are put down to nothing -- a worker ended for want of memory, say --
  and the job made the second time quarantines nothing.
- **A change lifts it.** A change to the object or to what it reads
  (`writeReaches`, as a job is dropped; by hand, an undo, not a recompute
  or the Animation) takes it out of the quarantine and its error off. It
  stays *risky* (`WorkerRecompute::risky()`): an edit's recompute that
  reaches it goes to a worker whatever it is expected to take
  (`editRecomputeIsLong`, `makeEditedFirst`, which then makes nothing
  here), so a second crash again costs only a worker. The Recompute
  command gives a recompute to a worker already. In the console nothing
  handed it over: a lifted object ran here (handed over since, sec 37).
- `WorkerRecompute::Info::crashed` (Python `recomputeInfo()["Crashed"]`),
  `WorkerRecompute::crashed(obj)`.

Not built: a sketch's execute gives geometry new ids, which no worker may
(sec 19), so a crashing sketch handed over comes back unfit and is made
here. The macOS crash reporter writes a report for each worker that dies.
Both done since: sec 36 and sec 35.

Tests: `TestWorkerRecompute.testACrashInAWorkerIsPutDownToItsObject` (a
chain Before -> Crash -> After, Crash a Python feature that sends itself
SIGSEGV in any process but the test's: quarantined, refused here, and
given to a worker again no worker dies), `
testAChangeToWhatItReadsHasItTriedAgainByAWorker`,
`testACrashAgainAfterAChangeIsRefusedAgain`,
`testADeathThatDoesNotComeAgainRefusesNothing` (the worker killed from
outside once: made by the next). GUI test
`tests/gui/crash-isolated-by-worker.py` (9 claims): the same chain through
the Recompute command -- the application goes on, the error shown in the
Report view and on the object, Recompute again kills no worker, the crash
fixed and Recompute made by a worker.

## 35. A worker's crash says what was on its stack (2026-11-21)

The user, 2026-11-21: "can we capture crash call stack for diagnostics for
both developer and end user". Before, the pool knew where in a job a worker
died (sec 34) and by which signal, and nothing of what the process was
doing; on macOS no crash handler was installed at all, in the worker or the
application, and the system's crash reporter wrote a report for every
worker that died.

- **The worker writes it, as it goes.** `WorkerCrash::install()`, first
  thing in `WorkerPool::serve()`: a handler of SIGSEGV, SIGBUS, SIGILL,
  SIGFPE, SIGABRT and SIGTRAP, run on a stack of its own (a crash for want
  of stack has none left). With nothing that allocates -- `write()`,
  `dladdr()`, Python's own traceback dump (`_Py_DumpTracebackThreads`, what
  faulthandler calls; `PyUnstable_DumpTracebackThreads` from 3.14) -- it
  writes to the worker's stderr, which is its log in the pool's directory:
  the signal, its code, the address, the sender for one sent; where in the
  job it was (`WorkerCrash::setAt()`, from `Request::at`); every Python
  thread's traceback; the frames of the thread that crashed, each with its
  image, the image's base and the nearest symbol. On macOS the frames are
  walked from the crash context (pc, lr, then the frame records, each
  checked to lie in the thread's stack): `backtrace()` run on the
  handler's stack, which is not the thread's, stops at once and gave no
  frame at all. Elsewhere `backtrace()`, primed at install.
- **It ends the process rather than die of the signal.** `_exit(128 +
  signal)`: a process killed by the signal is what makes macOS's
  ReportCrash write a report, eight of them for one test run before.
  `Workers/SystemCrashReport` (false) dies of the signal instead, for the
  system's report and a core where they are on.
- **The pool reads it back.** When a worker goes in the middle of a job
  (`crashOf()`), the last report in its log: how it ended is said from it
  ("crashed (signal 11: Segmentation fault)", where the exit code alone
  would say 139), the names demangled, the handler's own frames left out,
  and the text goes into the application's crash log (`Base::CrashLog`,
  `<UserAppData>/crash-<time>.log`, an entry "<doc>: worker N (pid P)
  crashed making job J (kind)"), into the result (`Result::stack`,
  `crashLog`; Python `Stack`, `CrashLog`) and, at log level, the Report
  view. The error says where: "its worker died over it (crashed (signal
  11: Segmentation fault)), at Crash; what was on its stack is in
  .../crash-2026_11_21-09_53_57_073.log", and so does a quarantined
  object's (sec 34): "... What was on the worker's stack is in ...". An end
  user has a file to send; a developer has the frames.
- **What it said last is read first.** A worker its handler ends is
  reaped at once, and the pool heard of the process's end before it had
  read the socket: its last `at` was still with the system, and the GUI
  test lost "at Crash" (8 claims of 10 failed). `finished()` now reads
  what is left on the socket before anything else.
- **The frames' lines, away from the main thread.** On the global thread
  pool (waited for at the exit, sec 33): `atos -o <image> -l <base>` for
  each image on macOS -- only where the developer tools are, since
  `/usr/bin/atos` without them asks the user to install them -- and
  `addr2line -e <image>` elsewhere; a return address one back, for the
  call's line. A second entry, "..., the frames with their lines": 0.3 s
  for an image here, `WorkerRecompute.cpp:576`, `Document.cpp:19620`.
  `Workers/CrashLines` (true).
- **The application's own crash, on macOS too.** `Application::init`
  installs `segmentation_fault_handler` on macOS as on Linux (SIGBUS
  added on both): the raw frames to stderr, then the crash log, then death
  by the signal -- the system's report and the dialog a user knows, as
  before. A worker replaces it with its own.

A report as the crash log has it, its lines found (the test's job
`test.fault`, `ctypes.string_at(0)` in a worker; cut):

    ===== 2026-11-21 10:20:46.015  CAUGHT  pid 32350  ...  WorkerRecompute:
      worker 0 (pid 32457) crashed making job 1 (test.fault), the frames
      with their lines =====
    signal 11: Segmentation fault, at address 0x0
    the thread that crashed:
    #0  _platform_strlen + 0x4  (libsystem_platform.dylib +0xe64)
    #1  string_at + 0x70  (_ctypes.cpython-312-darwin.so +0x98b0)
    ...
    #6  _PyEval_EvalFrameDefault + 0x31fe0  (libpython3.12.dylib +0x1e2590)
    #7  App::WorkerPool::serve() + 0x1ad8  (libFreeCADApp.dylib +0x4cc9d8)  WorkerPool.cpp:3162
    #8  (anonymous namespace)::serveWorkerPy(_object*, _object*) + 0x2c  (libFreeCADApp.dylib +0x63fb5c)  ApplicationPy.cpp:661
    ...
    #17  main + 0x498  (ThunderealCmd +0x3598)  MainCmd.cpp:134
    Python:
    Current thread 0x00000001ebb21e00 (most recent call first):
      File ".../python3.12/ctypes/__init__.py", line 525 in string_at
      File ".../Mod/Test/TestWorkerPool.py", line 54 in _fault
      File ".../Ext/freecad/worker.py", line 69 in _run
      File ".../Ext/freecad/worker.py", line 151 in serve

The walk follows the frame records: a function that keeps none, or was
reached by a tail call, is not there (#6 to #7 above skips the calls
between the job's Python and the worker's loop; not chased). A recompute's crash, in a Python feature's execute(), has
them all: `App::FeaturePythonImp::execute()` `FeaturePython.cpp:65`,
`App::DocumentObject::recompute()`, `App::Document::_recomputeFeature()`
`Document.cpp:19620`, `makeRecompute()` `WorkerRecompute.cpp:576`.

Not built then: Windows, where a worker had the CRT's handler, which wrote
its own crash log in the worker's user directory and threw. Built since:
sec 41.

Tests: `TestWorkerRecompute.testACrashSaysWhatWasOnItsStack` (a job reading
memory at 0: the result's How and Stack -- the address, Python's
`_fault`, the native frames down to `App::WorkerPool::serve()` -- the
crash log named in the error and holding the entry, then its lines entry
with `WorkerPool.cpp:` where a tool is, and no new system report).
`testACrashInAWorkerIsPutDownToItsObject` also checks the quarantine names
the file. By hand: `ThunderealCmd -c "os.kill(os.getpid(), SIGSEGV)"`
writes the crash log, exits 139, and macOS's report is made.

## 36. A worker's recompute that gives new geometry ids is taken in (2026-11-21)

The user, 2026-11-21: "Sketcher rebuild external geometry is a separate
step from constraint solving. check if it is possible to split the task or
find a way to sync geo id from worker, because their id counter is local
scope only".

What gives the numbers. A sketch's execute() is `rebuildExternalGeometry()`
then the solve. The solve gives none: geometry keeps its ids. The rebuild
gives a new id only to a piece of a projection no old piece stands for --
a projected face come out in more edges than before, a reference added; a
piece takes the id of the old one named after the same element, then of
the unnamed ones in order. The counter (`Document::nextGeoId`) is the
object's own and file wide (`FileHistory::_lastGeoIds`, kept above every id
the object held in any version or branch), as is `nextObjectCount`, a
body's sheet metal walls and bends. A worker's copy reads them from the
store as of the last save: past them in memory here -- ids given to pieces
since undone -- it would give an id this process gave. Before, any number
given made the job unfit ("the recompute gave things new numbers"), and the
whole recompute was made here.

Splitting the task -- the rebuild here, the solve there -- would make every
sketch two steps in two processes, with the solve waiting on what the
projection reads, and keep the problem for anything else that numbers. The
counters are synced instead:

- **The worker counts on from what this process gave.** The job's inputs
  carry, for each object the recompute reaches, what it gave so far
  (`Document::givenNumbers()`: kind -1 the last geometry id, 0, 1, ... its
  counts). The worker raises its own to them (`noteGivenNumber()`, which
  never lowers one) before it recomputes.
- **It says what it gave.** `RecomputeHook::noteMinted(obj, kind, before,
  after)`, from `nextGeoId` and `nextObjectCount`; a
  `RecomputeHook::Minting` living on the thread notes, per object and
  kind, the count it went from first and to last. The result's output says
  them: `numbers`, [name, kind, from, to]. An object made here that gave
  numbers there (`madeHere`, TechDraw) still makes the job unfit: it gives
  its own here.
- **This process takes them if it gave none since.** Before the result is
  put in (`takeNumbers()`, beside the strings): each object's count here
  must be at most `from` -- else it gave numbers since the job was sent,
  which may be the same numbers for other things, and the result is not
  put in ("Sketch gave numbers here since"). Then each count is raised to
  `to`: what this process gives next comes after the worker's.

Tests: `TestWorkerRecompute.testASketchsNewGeometryIdsAreTakenFromAWorker`
(a sketch projecting a Python feature's face, 4 sides, then 6: a worker
makes it, nothing unfit or made here; 6 distinct ids, the 4 kept, the 2
new above them; a line added here then gets an id above the worker's),
`testAWorkerCountsOnFromIdsGivenHereAndUndone` (6 sides made here and
undone: the worker's new ids are above the undone ones). The second, with
the counts not sent, fails: the worker gives the undone ids again, the
result is refused, and the recompute is made here.

## 37. A script's recompute of what crashed is a worker's first (2026-11-21)

The other limit of sec 34: an object taken out of its quarantine by a
change is risky, and the GUI hands an edit's recompute that reaches it to a
worker; a `doc.recompute()` -- the console, a script, a macro, a command
that recomputes at once -- ran it here, where a crash again is the
application's.

`Document::recompute()` offers the recompute first
(`RecomputeHook::handOver()`, before it marks the document recomputing;
not while a hook is installed, so not a result being put in nor a worker's
own). `WorkerRecompute`'s `handOverRisky()`: when what it reaches holds an
object that crashed a worker before and is not refused now, the recompute
is started with a worker and waited for (`WorkerRecompute::start`, `wait`,
the pool polled); the result is put in; what is left is made here, as
ever. A crash again quarantines the object again, and the recompute here
leaves it in its error. One still refused needs no worker -- it is not
executed anywhere -- and a recompute that cannot be started (no workers,
the pool stopped) is made here. `Info::handedOver`, Python
`recomputeInfo()["HandedOver"]`; `Workers/HandOverRisky` (true).

Test: `TestWorkerRecompute.testAScriptsRecomputeOfWhatCrashedIsAWorkersFirst`
(quarantined, a `doc.recompute()` is not handed over; changed but still
crashing, it is, the worker dies twice and the object is refused again,
nothing ran it here; fixed, the script's recompute is a worker's, and what
reads it is made; with the option off, made here).

The recompute of one object, later the same day: `Document::recomputeFeature(obj)`
-- what a task dialog's preview calls on each change of a parameter, and
a script's `obj.recompute()` -- executed it here. It is offered too, unless
the document is recomputing (asked from another object's execute(), whose
recompute was offered already). `handOver()` now says whether a worker made
it: a result put in, or a death again (the object refused here). Made and
up to date, the call returns; otherwise it goes on as before, and a
refused object is left in its error, not executed. A worker's recompute
of one object also makes what it reads that is touched, as any
recompute of the document asked for that object does. Test
`testOneObjectsRecomputeOfWhatCrashedIsAWorkersFirst` (quarantined, not
handed over; changed but still crashing, handed over, the worker dies, it
is refused again; fixed, made by a worker; option off, made here). A
control build without the `Document.cpp` change fails it: nothing is
handed over and the object runs here.

## 38. Splitting one recompute over workers: measured, not built (2026-11-21)

The question: one recompute made by several workers at once, each taking a
part of what it reaches. Measured on scanner.FCStd (686 objects, the one
large real model here) before building anything, by a console probe
(`critical.py`): a recompute here, each object timed by the gap between
recomputed signals, then over the dependency graph of what was recomputed
the critical path -- the longest chain by cost, which no split beats --
and a list schedule over k workers (ready objects by longest path left),
with 0.3 s charged where an object reads one made on another worker.

| recompute            | objects | here    | critical path      | bound | 2/3/4/8 workers |
|----------------------|---------|---------|--------------------|-------|-----------------|
| as opened            | 466     | 10.30 s | 6.48 s, 9 objects  | 1.59x | 6.48 s each     |
| every object touched | 625     | 10.49 s | 6.58 s, 10 objects | 1.59x | 6.88 s with hops |

One feature is the path: `PolarPattern003`, 1024 copies of a pad around an
axis (5129 faces), 6.46 s of the 10.30. Everything else -- 3.8 s, 37% --
could run beside it, and two workers already reach the bound; more add
nothing. The earlier grouping by connected parts (2026-11-17) found 1.0x,
which was too strict: 520 of the 527 reached objects are connected, but
not in one chain.

What the 1.59x would cost, measured the same day:

- Memory: a worker recomputing scanner peaks at 963 MB resident, this
  process at 425 MB. Resident is not what the worker costs: it counts
  shared library pages and memory malloc has freed but kept
  (reclaimable). Its footprint (`footprint -p`, dirty and compressed
  memory it owns -- what macOS charges it), probe `wfoot.py`: sampled
  every half second or so during the recompute, at most 459 MB (resident
  694 MB then; resident peaked at 910 MB in that run); idle after, kept
  for the next job, 257 MB against 878 MB resident, of which 507 MB was
  reclaimable malloc memory. This process: 207 MB after the open, 239 MB
  after the put-in. A second worker is then a quarter to a half gigabyte,
  not one; the sampling may miss the top of the peak.
- Cores: the pattern alone is 6.58 s wall and 13.66 s CPU -- OCCT's
  parallel boolean already uses two cores on average of the 8. The rest
  would run on the cores left, so the bound is not taken from the pattern.
- The result: two workers' results are one recompute here. Each put-in is
  today one file into one recompute (sec 19), its numbers checked against
  one worker's (sec 36), its ids from one lease (sec 32); a split puts in
  two, or waits for both.
- Its use: a recompute by a worker does not hold the application already
  (sec 19, 28); a split brings only the result closer -- here 10.3 s to
  about 6.5 s plus the second put-in.

Not built. The gain is bounded by the costliest single object, on the one
model measured by one feature, and a split cannot cut an object. For that
feature the other way was measured too: with `ParallelTransform` off its
1024 copies are fused one at a time -- 394 s wall, 565 s CPU -- so the
model already takes the fast way; what is left in it is OCCT's fuse of
1024 solids.

Seconds saved, not the ratio (the user, the same day): a split is worth
what it takes off a recompute someone waits for, and that is mostly an
edit's. The probe again, every object touched (10.47 s here over 625
objects), then each of the 625 taken as an edit: what it reaches (it and
what uses it) made here, against that reach's critical path. No cost for a
second worker's job or put-in is counted.

| saved by a split | edits |
|------------------|-------|
| >= 0.25 s        | 22 (4%) |
| >= 0.5 s         | 13 (2%) |
| >= 1 s           | 4 (1%)  |
| >= 2 s           | 3       |
| >= 3 s           | 2       |

Of the 27 edits whose recompute takes 1 s or more here, the median saves
0.33 s. The most:

| edited             | reach | here   | split  | saved  |
|--------------------|-------|--------|--------|--------|
| XY_Plane007        | 294   | 9.63 s | 6.57 s | 3.06 s |
| Spreadsheet        | 284   | 9.61 s | 6.57 s | 3.03 s |
| Z_Axis007          | 138   | 8.95 s | 6.56 s | 2.39 s |
| Sketch033          | 130   | 8.24 s | 6.57 s | 1.67 s |
| Pad018             | 92    | 7.22 s | 6.57 s | 0.65 s |
| Sketch039          | 119   | 1.34 s | 0.81 s | 0.53 s |

The edits that save seconds are the ones above everything: the
spreadsheet of parameters, an origin plane, the body's first sketches --
and each is a recompute of 8 to 10 s, of which the pattern is 6.5. An
edit inside one branch saves what runs beside it in that branch, a few
tenths at most here.

## 39. TechDraw made inline, and by a worker (2026-11-21)

The user: TechDraw makes most of its recompute in threads; a switch to make
it inline, and a worker to make it where the time says so.

What was there. A view's execute() starts its projection (hidden lines),
then its faces, a section's cut, a detail's piece, a complex section's
aligned pieces, each on Qt's global thread pool, and returns; the rest of
the view is made in the watcher's `finished()`, which the event loop
delivers. The result ends in `ProjectedGeometry`
(docs/TechDrawStoredGeometry.md), written as the last thread ends. Which is
why workers left TechDraw alone (`RecomputeHereTypes`, sec 19): a worker
is a console process, and a console process runs no event loop -- the pool
makes a `QCoreApplication` for its sockets, and nothing dispatches its
events -- so `finished()` never came.

Measured before the change, scanner.FCStd (35 views that project):

- Console, every view touched, `doc.recompute()`: the recompute returned
  after 10.35 s, and no view was ever made -- 0 of 35, Qt events pumped
  for 300 s after. A script or a console that recomputes a drawing gets
  none.
- GUI, the same: the recompute returned after 14.53 s, the threads of 34
  views ended by 14.87 s -- each within about a second of its execute(),
  overlapping the recompute. (`SectionView003` never ends there; the
  three views not recomputed wrote their stored projection once, early,
  at 1.6 s.)

Built. `TechDraw::startMaking(watcher, work, finished)` (`StartMaking.h`)
is the one place the five sites start a thread: `work` on the pool and
`finished` from the watcher as before, or -- where
`DrawView::makesInline()` -- both at once, on the recomputing thread, so
the view is made when its execute() returns. `makesInline()` is always
true without a `QGuiApplication` (a console, a worker), and in the GUI it
is `Preferences::makeInline()`, `Mod/TechDraw/General/MakeInline`
(default false). A failure is told as from a thread, by the progress the
work cancels; one reported while the view is in its own recompute is its
return (`DrawView::m_heldError`, already there).

Measured after, console: the recompute returns after 12.78 s with 31 of
the 35 views made (the four left: `SectionView003`, and the three not
recomputed). The 2.4 s is what the drawings cost made inline, one after
another, where the GUI's threads overlap them with the rest. Tests: the
TechDraw GUI tests td-edit-two-pages 30, td-model-interleave 40,
td-stored-hatch 9, td-undo-stored-projection 13 pass every claim both
with the preference off and on; TestTechDrawApp 25 OK.

A worker makes the views, the same day. What is made here is no longer
every `TechDraw::` type but what `DocumentObject::resultInProperties()`
says is not all in its properties -- the rule an undo already goes by
(docs/MultiViewEdit.md sec 12.10): a TechDraw view whose projection is not
stored (`StoreProjectedGeometry` off), or one still being made.
`RecomputeHereTypes` stays, now empty by default (`TechDraw::` for as
before). In the worker the view is made inline (above) and its projection
stored; here its values are put in, and the view installs them as after an
undo: `DrawViewPart::onChanged` notes `ProjectedGeometry` written under
`RecomputeHook::puttingIn()`, and its `recompute()` override, once the
put-in is over, runs `installStored()` -- the threads of another state
dropped, the stored projection restored (a section's cut, a detail's piece
beside it, docs/TechDrawStoredGeometry.md sec 8), the view and its
dimensions and balloons repainted. Nothing is projected here.

And the time. Here, in the GUI, a view's recompute returns after it starts
its threads, so the gaps between recomputed signals counted only that
start. `WorkerRecompute::noteSeconds(obj, seconds)` sets an object's time
where its making runs on after its recompute; the view notes the time
from its recompute's start to its last thread's end (`noteMade()`, at the
two places its geometry is complete). An edit that reaches a slow drawing
is then expected to take what it does, and goes to a worker by the same
threshold as the rest (sec 28).

Tests: `testATechDrawViewIsMadeByAWorker` (a box's view made by a worker:
Applied, nothing made here, the edges this process makes; with the store
off, made here); a control run with `RecomputeHereTypes` = `TechDraw::`
fails it (the page and the view made here). GUI
`tests/gui/td-recompute-by-worker.py`, 6 claims, three runs: a plate with
144 holes, its view's threads 0.163 s after its recompute returned and the
expected time 0.168 s; the Recompute command after an edit goes to a
worker, nothing is made here, the projection is written only at the
put-in (three writes in one moment: the property reads back in parts,
`Restore` and `RestoreDocFile`), 441 edges, the same as this process
makes. A control build without the noted time fails the first claim: the
view is expected to take 0.004 s.

What all of scanner's TechDraw costs, measured again when the user asked
"for all the pages?": the numbers above timed only the 35 views that
project, to their projection's write. The file has 10 pages and 229
TechDraw objects -- 147 dimensions, 23 projection-group items in 9 groups,
10 sections, 10 images, 7 balloons, a detail, a part view. GUI probe
(`tdpages_gui.py`): every TechDraw object touched, the time until no
projection is written and the main thread (a 10 ms heartbeat) has been
free for 3 s:

| | recompute returned | quiet | main thread blocked |
|---|---|---|---|
| pages closed (the model's own pending recompute in it) | 12.68 s | 13.19 s | 12.80 s |
| the 10 pages opened | -- | 1.93 s | 0.62 s |
| pages open, TechDraw only | 0.95 s | 1.20 s | 1.09 s |
| pages open, every object | 13.04 s | 13.29 s | 13.06 s |

With the pages open, the TechDraw recompute is 0.95 s on the main thread
-- 242 dimension recomputes 0.31 s, 20 projection-group items 0.46 s (the
start of their projections), 15 group recomputes 0.12 s -- and its threads
and the pages' drawing end 0.25 s later. 31 projections are written; not
the three on Page009, whose source `Fillet011` stays touched with its
schema migration pending, so they are never recomputed and draw what they
stored, nor `SectionView003`, whose cut fails. So on this model all of
its drawings cost about a second, eight cores sharing the threads; one
model, of mid-sized bodies -- a drawing of a large assembly is another
matter, not measured here.

On scanner.FCStd, every object touched, a worker's recompute (console,
14.86 s): Applied, nothing made here, 622 objects supplied; of the 35
views, 34 have edges, and visible and hidden edge counts are those this
process makes of each, all 35 (`SectionView003` none, as here).

## 40. The Windows run (2026-11-21)

Worker at 85eb204c7a built and run on Windows 10 (MSVC 14.44, conda
RelWithDebInfo) by the remote session on that box. Two compile errors came
first: `SIGBUS` in `WorkerCrash::what()`, which Windows does not have
(eb7800cda0), and the timeline's lambda named `near`, a macro of the
Windows headers -- main's fix ba6567b985, which Worker lacked, picked
(85eb204c7a). Then: ctest 1267/1267; Document 363, TestSketcherApp 182,
TestAnimation 78 OK; TestPartDesignApp 1072/1073, the one
`TestSheetMetalConvert.testSolidOutside`, failing on main there since
2026-11-16; TestWorkerPool 63/66; the worker GUI tests all pass but two
claims. Every failure was the tests' own, made on POSIX:

- `os.kill(pid, 0)` probes nothing on Windows -- signal 0 is
  `CTRL_C_EVENT`, sent to the console -- so a gone worker never looked
  gone. Tests now ask `_alive(pid)`: `OpenProcess` and
  `GetExitCodeProcess` there.
- `os.kill(os.getpid(), signal.SIGSEGV)` is `TerminateProcess` there: an
  exit with code 11, not a crash. And `ctypes.string_at(0)` does not crash
  either: ctypes runs a foreign call under its own exception handler and
  raises `OSError`. `_crash()` reads at 0 through `faulthandler._read_null()`
  on Windows, which is unguarded.
- A real crash in a Windows worker is told as `crashed (0xc0000005)` with
  no stack and no crash log: the worker has no handler there yet (sec 35).
  The tests say that is so, rather than leaving it out.
- animation-workers counted the ruler labels' ClearType fringes as the
  amber "being made" mark. Colours now count only in runs of three or
  more pixels along a row, which the cache line's frame-wide pieces are
  and a fringe is not.
- The permission test failed in the user's own profile (and passed in a
  clean one): it now revokes what the profile keeps for its principal --
  the same for every new empty document -- before it grants.

Found beside, and fixed: `Base::FileInfo::renameFile()` used `_wrename`,
which on Windows refuses a target that exists where POSIX `rename`
replaces it ("Permission denied (13)"), and `GrantStore::save` used
`std::rename`, the same: on Windows a `grants.json` that existed was never
updated. `renameFile()` is `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` there
now, and the store renames through it.

The permission test failed again in the user's own profile with the
revoke in, and the remote session bisected it to the profile's Pyodide
install: with a runtime there, expressions are evaluated in the sandbox's
image, whose `FreeCAD` facade has `listDocuments`, `getDocument` and the
rest of its app.query calls but no `Version()` -- "No attribute named
'Version'". Reproduced here with `FREECAD_USER_HOME` the Pyodide home. The
test now reads `_app.getDocument(<<name>>).Name`, gated by app.query in
both engines, and passes in both homes, refused after the revoke by each
engine where it should be. The image's missing `Version()` is a gap
between the two engines, not the worker's; left for the sandbox work.

A Windows Error Reporting dialog would hold a crashed worker's end until
someone answered it. None was seen, but no worker had really crashed
there (above), and `faulthandler._read_null()` turns the dialog off
itself, so the tests cannot tell. A worker turns it off at
`WorkerCrash::install()` (`SetErrorMode(SEM_NOGPFAULTERRORBOX)`, MSVC's
abort report too), unless `Workers/SystemCrashReport`: the counterpart of
the macOS report a worker no longer makes.

## 41. A Windows worker's crash says what was on its stack (2026-11-21)

Sec 35 on Windows: a worker that crashes there writes the same report to
its log, the pool reads it back with the same `WorkerCrash::read()`, and
the crash log, the result's `Stack`/`CrashLog` and the error and
quarantine messages name it as on macOS. Built and measured on the
Windows 10 box (MSVC 14.44, conda RelWithDebInfo).

- **First, a worker did not crash at all.** `Application::init` on MSVC
  sets `std::signal(SIGSEGV, segmentation_fault_handler)`, which the C
  runtime's frame around `main()` calls for an access violation, and the
  handler throws `Base::AccessViolation`; this build (`HAVE_SEH`, not
  `_DEBUG`) also sets an SE translator on the main thread, the one a
  worker serves its jobs on. A worker's fault came back as a job that
  failed -- "Illegal storage access! Please save your work under a new
  file name and restart the application!", Died false -- and a crash in a
  feature's execute() as a recompute that went on (the GUI test's object
  even ended Up-to-date). `WorkerCrash::install()` hands the worker back
  to the system: SIGSEGV, SIGILL and SIGFPE at `SIG_DFL`, SIGABRT to the
  worker's own handler, `_set_se_translator(nullptr)`. The main process
  keeps both, for the GUI's sake.
- **The handler is the unhandled exception filter**
  (`SetUnhandledExceptionFilter`), not a vectored one: that one hears
  first-chance exceptions something may yet catch, as ctypes does around
  every foreign call. `SetThreadStackGuarantee(64 KB)` on the serving
  thread leaves room for a report of a stack overflow. It writes, with
  `WriteFile` to the worker's stderr and nothing that allocates, the lines
  of sec 35: `signal` the one a POSIX worker would have died of (an access
  violation, a page not read in, a stack overflow 11; an illegal or
  privileged instruction SIGILL; the divide and float codes SIGFPE),
  `code` the exception's in hex, `address` the address denied (the
  exception's second word) for an access violation and the faulting
  instruction's otherwise; `at`; Python's dump (`_Py_DumpTracebackThreads`
  from the DLL that holds `Py_Initialize`, to fd 2); then the frames,
  unwound from the exception's context by the images' own unwind data
  (`RtlLookupFunctionEntry`, `RtlVirtualUnwind`; a frame with none, a
  leaf or a call through a null pointer, takes its return address from
  the top of the stack). Each frame has its image's base and path and the
  nearest export below it -- none past 64 KB, since FreeCADApp.dll
  exports few of its functions and the nearest named one was megabytes
  away ("PropertyLinkSubList::Save + 0x9c2f9a").
- **It ends the process with the exception's code**
  (`TerminateProcess(code)`): QProcess has that as a crash, and no
  report is made. `Workers/SystemCrashReport` hands the exception on to
  Windows Error Reporting after the report is written. `abort()` (and
  `std::terminate()` through it) is reported the same way, from
  `RtlCaptureContext`, signal 22, code 0.
- **The pool tells it by its code.** `what()` says "exception
  0xc0000005 (access violation), at address 0x0"; How is "crashed
  (0xc0000005)", as `endedHow()` said without a report; the quarantine
  "crashed a worker process twice (0xc0000005)". POSIX output is as it
  was, byte for byte. `read()` takes a code in decimal or hex, and lines
  ending in "\r\n" (Python's dump goes through the C runtime's text mode).
- **The frames' names and lines from the PDBs**, on the thread pool as
  atos and addr2line are (sec 35): dbghelp in the main process, on a
  session of its own, each image loaded at the base it had in the worker
  (`SymLoadModuleExW`), then `SymFromAddrW` and `SymGetLineFromAddrW64`
  -- the function's real name, its offset, "File.cpp:line". An image with
  no PDB keeps what the worker said, so an end user's build without PDBs
  gives no error. 1 s for a report here, FreeCADApp.pdb loaded. dbghelp
  is not thread safe: its calls here (and MSVC name demangling) are under
  one lock.

A report as the crash log has it (the test's job `test.fault`,
`faulthandler._read_null()` in a worker; the worker's entry, cut):

    ===== 2026-11-21 23:11:04.832  CAUGHT  pid 60944  ...  WorkerRecompute:
      worker 0 (pid 57472) crashed making job 1 (test.fault) =====
    exception 0xc0000005 (access violation), at address 0x0
    the thread that crashed:
    #0  0x7ffdae200a79  (python312.dll +0x80a79)
    #1  PyCFunction_GetFlags + 0xc29  (python312.dll +0x161389)
    ...
    #9  0x7ffd9885cd60  (FreeCADApp.dll +0xa2cd60)
    #10  0x7ffd98862b2f  (FreeCADApp.dll +0xa32b2f)
    ...
    Python:
    Current thread 0x0000f300 (most recent call first):
      File ".../Mod/Test/TestWorkerPool.py", line 57 in _crash
      File ".../Mod/Test/TestWorkerPool.py", line 89 in _fault
      File ".../Ext/freecad/worker.py", line 69 in _run
      File ".../Ext/freecad/worker.py", line 151 in serve

and a second later, "..., the frames with their lines":

    #0  faulthandler_read_null + 0x29  (python312.dll +0x80a79)  faulthandler.c:1004
    #1  cfunction_vectorcall_NOARGS + 0x109  (python312.dll +0x161389)  methodobject.c:481
    ...
    #8  Py::Callable::apply + 0x1b  (FreeCADBase.dll +0x7bbab)  Objects.hxx:3257
    #9  `anonymous namespace'::runPython + 0x2e0  (FreeCADApp.dll +0xa2cd60)  WorkerPool.cpp:2756
    #10  App::WorkerPool::serve + 0x277f  (FreeCADApp.dll +0xa32b2f)  WorkerPool.cpp:3177
    #11  `anonymous namespace'::serveWorkerPy + 0x1f  (FreeCADApp.dll +0xc53c4f)  ApplicationPy.cpp:661
    ...
    #23  main + 0x743  (ThunderealCmd.exe +0x29663)  MainCmd.cpp:134

The unwind has every frame down to `RtlUserThreadStart`, the calls
between the job's Python and the worker's loop included (macOS's walk
skips them, sec 35). The worker's own names are rough even where an image
exports much: python312.dll's "#1 PyCFunction_GetFlags + 0xc29" is
`cfunction_vectorcall_NOARGS`. The lines' entry is the one to read.

Windows Error Reporting, measured with a job reading memory at 16 through
`ctypes.c_int.from_address(16).value` -- ctypes' own code, outside the
guard it puts around foreign calls (`kernel32.RaiseException` through
ctypes is caught: "exception: access violation writing ...", the worker
goes on) and not `faulthandler`, which turns the reporting dialog off
itself:

- `Workers/SystemCrashReport` false (the default): the report as above,
  "crashed (0xc0000005)"; no WerFault.exe ran, no report in
  ReportArchive or ReportQueue, no dump, no Application event. The job
  failed in 1.6 s.
- true: the report as above, then WerFault.exe (three of them for the
  two workers the job crashed -- a crashed job is given a second worker),
  two APPCRASH reports in `%ProgramData%\Microsoft\Windows\WER\ReportArchive`,
  Application events 1000 and 1001 ("Faulting module name: _ctypes.pyd
  ... Exception code: 0xc0000005"). No dialog was shown, and no dump
  (LocalDumps is not set on that box). The job took 11.5 s: WER held
  each dying worker for some seconds.

Not done: a crash in the handler is passed on to the system (the flag
`entered`); `__fastfail` (the C runtime's invalid-parameter and purecall
ends) reaches no handler at all; x64 only (a 32-bit or ARM64 worker
writes the report without frames).

Tests: `testACrashSaysWhatWasOnItsStack` on Windows: How "crashed
(0xc0000005)", the address, `_fault`, a FreeCADApp.dll frame, the crash
log named and holding the entry, then, where FreeCADApp.pdb is beside the
DLL, the lines' entry with `App::WorkerPool::serve` and `WorkerPool.cpp:`;
no new Windows Error Reporting report or dump (`systemCrashReports()`
looks there now). `testACrashInAWorkerIsPutDownToItsObject`: the
quarantine names the crash log, "(0xc0000005" for "(signal 11".
crash-isolated-by-worker's claim of the crash log is the same on Windows.
Run there: TestWorkerPool 67/67, Document 363, TestTechDrawApp 25, ctest
1267/1267, crash-isolated-by-worker 10/10, animation-workers,
td-recompute-by-worker, td-stored-hatch, td-undo-stored-projection.

## 42. A recompute split over two workers (2026-11-22)

The user, after sec 38: "Two way split. Go".

**Which halves.** At `WorkerRecompute::start()`, `splitOf()` takes what
the recompute reaches, each object's time as `expectedSeconds()` knows it
(the last here or by a worker, else the log's records, else 0), and the
dependency edges among them. It finds the longest chain by those times
and tries a cut at each of its objects: the first half is the cut, the
chain after it, everything that depends on them, and what they read in
the recompute; the second half is all the rest and what it reads. What
both read -- the chain before the cut among it, a spreadsheet, the
origins -- is in both, made twice. The cut that saves most wins: the
whole's time less the larger half's. A cut at the chain's start would
take everything (all reads the spreadsheet), which is why the cut is
looked for along the chain and not only at it.

It is split when that saving is at least `Workers/SplitSeconds` (1.0 s
then, 5.0 s since 2026-11-22 at the user's word, 2.0 s since 2026-11-23,
sec 59; 0 never splits) and the pool may have two workers. Then two jobs, each
`objects` its half, the same `touched`, `display`, `numbers` and
`refused` cut to it; a worker recomputes exactly what it is sent
(`doc->recompute(objs)`), so the two make their halves of the same
state at once.

**The pool.** It grew by one worker only when more was queued than there
were workers: one half running and one queued is not, and the second half
waited for the first. It now counts the jobs running too
(`WorkerPool::Private::grow`). The other rules stay -- one at a time, once
each worker has made a job, and the memory left must hold one more like
the biggest.

**Putting in.** `State::second` holds the second half's job and result.
`done()` keeps each half's result; the put-in waits until neither half is
running (`making()`), then `supplierFor()` reads the first half's file and
adds the second's (`Supplier::addsOf`, `merge`): only the objects the first
did not make -- what both made is the first's -- their numbers checked
and taken for those only (sec 36; what both made counted the same numbers
from the same start, and the second's would be refused), their strings
taken by id (by content only for one table: a second half that sent none
by id is not taken, and its objects are executed here, as any a worker did
not make). One recompute puts all in. Each worker is told what of it is
its own (`putIn`, sec 29). Either half failing leaves the other put in.
Found by its test and fixed: a put-in did not ask the quarantine (sec 34
asks it only outside a hook), and with one half's worker dead in an object
that object, having no value from the other half, was executed here -- in
this process, which it crashes. The supplier now refuses what is
quarantined. A recompute run here meanwhile claims both jobs.

Info `split`, `splitSaving`, `splitFirst`/`splitSecond` (objects of each
half), `suppliedSecond`; Python `Split`, `SplitSaving`, `SplitObjects`,
`SuppliedSecond`.

**Measured, scanner.FCStd, console** (`fcad-probes/worker/splitprobe.py`),
every object touched, 686 reached, 625 made:

| | from start to put in | worker | supplied (second half) |
|---|---|---|---|
| here | 12.84 s | -- | -- |
| one worker, cold | 16.27 s | 12.78 s | 625 |
| one worker, warm | 13.44 s | 12.85 s | 625 |
| split, second worker cold | 11.64 s | 10.93 s | 625 (270) |
| split, warm | 11.64 s | 10.94 s | 625 (270) |

The halves were 382 and 661 objects, 418 of them in both. Expected to
save 2.79 s, it saved 1.80 s (13.44 to 11.64): the larger half took 10.93
s where the times said 10.06 -- presumably two workers and the pattern's
own threads sharing the eight cores; not chased. A cold second worker cost
nothing seen: the time is the same cold or warm, its start under the
first half's.
What both made, compared object by object (volume, area, faces, edges;
views' visible and hidden edges): the split's results are the one
worker's, all 440; both differ from here in the same six, the `Local_CS`
coordinate systems, whose volume is noise on an infinite plane (1e167).

A `touch()` of the spreadsheet alone reached 6 objects made, not the 284
of sec 38's table: a touch that changes no value changes nothing that
reads it. The edits sec 38 counted are real changes of a value; not
measured here.

Tests: `testARecomputeIsSplitOverTwoWorkers` (two slow branches under
what both read: split, each made by a worker of its own, at once, put in
as one, what both read from the first half; three runs),
`testACrashInOneHalfIsPutDownToItsObject` (the second half's worker dies
in Right twice: the first half put in, Right quarantined and not run here,
what reads it not made from it), `testARecomputeThatSavesLittleIsNotSplit`.

Not done: more than two halves; a half's objects made in the other's
worker when that one is gone; a split of an edit's recompute measured in
the GUI on a real change.

## 43. Page009's views: two references lost in the migration (2026-11-22)

Sec 39 left the three views on scanner's Page009 unmeasured: their source
`Fillet011` stayed touched with its migration pending, so they were never
made. Nothing in TechDraw or the worker kept them back. `Fillet011` needs
`Pocket041`, and `Pocket041` failed ("Part::FaceMaker: result shape is
null"), so nothing after it was made; nor anything after `Helix001`
("Sub shape not found: scanner#Helix.?Face24"). Both are references
followed by geometry, wrong in two ways.

The file was written by OCCT 7.7.2, and its element map version (1.15
.70200.4) is not this build's (1.17.80001.4). The migration recompute
makes every name again, and each reference is found again by the
geometry its element had (`PropertyLinkBase::_updateElementReference`,
`TopoShape::searchSubShape` with `CheckGeometry`). The geometry was the
same; the search was what failed:

- **Two halves of one circle were one edge to it.** `Pocket041`'s
  profile is four edges of `Pocket040`, two pairs of half circles. Each
  half has the other's two vertices, and its trimmed curve, 0 to pi on a
  circle turned half a turn, compares the same as the other's. The
  search returned both, and the reference took the first:
  `Edge78 -> Edge76`, `Edge59 -> Edge58`. The profile became two edges
  twice, and no face. The same holds for the two halves of a cylinder.
  Now a matched edge must also hold the middle of the one searched for,
  and so must each edge of a matched face's outline. A face is matched
  by its outline, and that stays: a point inside the face, tried first,
  fell in the hole a cut had made in the top of a box, and the top was
  no longer found (`ShapeDiffPlainTest.aHoleMoreInAShapeWithNoNames`,
  three `ElementAppearance` tests and three `TransactionBranchCases`).
- **A circle written as a B-spline was another edge.** `Helix001` is on
  `Face24` of `Helix`, a plane face with a circular edge, which OCCT
  8.0 makes as a B-spline of degree 6. `isSame` compares the two ways of
  writing the curve and says no, so the face was not found. Now an edge
  whose curve is written otherwise is the same edge when its vertices
  match and points a quarter, half and three quarters along each lie on
  the other. That holds for an edge searched for itself and for the
  edges of a face.

On scanner, every object recomputed in the console: 524 made, where 25
had stayed touched before and 4 now are: `SectionView003` and
`Dimension012`, which fail, `Page004`, which holds them, and `Group`.
Three are invalid, all three failures of the model's own:
`SectionView003`, whose cut fails; `Dimension005`, on the section that
draws nothing; and `Dimension012`, a distance on an edge that is now a
B-spline. Every shape's volume and area are the file's, to four places
(`LinkGroup001` to 4e-8 relative). 24 objects have fewer or more faces
than the file: OCCT 8 splits faces otherwise. A reference that took the
wrong edge would show in the volume of a fillet or a pocket, and none
does. Of 99 references changed in the migration, 30 moved to another
element number. All are right by that measure.

Tests (`tests/src/Mod/Part/App/TopoShapeSearch.cpp`):
`halvesAreToldApart` (the halves of a circle and of a cylinder, each
finds itself alone) and `anArcWrittenAsABSplineIsFound` (the arc and a
face bounded by it found as their B-spline form; an arc of another
circle between the same two points is not). Both fail without the
change.

Page009's three views are now made, here and by a worker alike (93, 45
and 43 visible edges, none hidden, both ways). Its 25 dimensions are not
alike, and that is two more problems, open:

- **Here, some dimensions now measure something else.** When a view is
  made again, TechDraw finds its dimensions' references again in the new
  projection ("no exact match for changed 2d reference" in the log) and
  rewrites their `References2D`. With the model's geometry unchanged, a
  dimension should keep its value. `Dimension246` (3.5978) and
  `Dimension259` (3.6) do; `Dimension264` goes from 15.9821 to 4.4245
  and `Dimension265` from 18.3095 to 24.2612. Their references were
  found again wrong.
- **A worker's result is not here's.** After the worker's put-in, the
  dimensions keep their old `References2D`. With every object touched,
  five of the 25 come back invalid (`Dimension264` to `267`, `274`), and
  most others with other values. With only what the file left touched,
  `Dimension264` comes back valid but reading 0.0, and `Dimension259`
  17.5. The dimension's own `execute()` rewrites its references
  (`fixByName`, `fixExactMatch`, `handleNoExactMatch`); whether the
  worker's put-in leaves that write out, or the worker finds them again
  otherwise, is not yet known.

## 44. A split into more parts; a spreadsheet's edit made one behind (2026-11-22)

**More than two parts.** `splitParts()` begins with one part, everything
the recompute reaches, and splits the costliest part in two (`splitOf`,
sec 42) while that brings the costliest of all the parts down by at least
`Workers/SplitSeconds`, and while there are fewer parts than the pool may
have workers. Only the costliest part decides when the result comes, so
splitting any other gains nothing. Splitting the part that holds the
longest chain gives nothing either: a chain cut in two still makes all of
it in one part, since that part reads what comes before the cut. So in
practice it is the rest that splits further. Each part is a job of its own (`State::parts`);
all are put in as one recompute when all are over: the first part with a
result is the base, each other one added as the second was (`addsOf`,
`merge`, its numbers only for what it adds, its strings by id). Each
part's worker is told what is its own (`putIn`). `SplitObjects` in
`recomputeInfo()` gives each part's size; `SuppliedSecond` counts the
objects made from any part but the first.

**`Workers/SplitSeconds` defaults to 5 s** (the user, 2026-11-22; it was
1 s): a split costs a second worker's memory and a second put-in, and is
worth it where it takes seconds off a wait. The tests set 1 s for what
they split.

Test `testARecomputeIsSplitOverThreeWorkers`: three branches of 2 s each
under what all read, three workers allowed. The first such recompute
grows the pool, and the second, timed, is split in three, expected to
save 4 s. Each branch is made by a worker of its own, in under two
branches' time. `testARecomputeThatSavesLittleIsNotSplit` now checks
three settings: the 5 s default, 2 s and 0.

**A spreadsheet's edit, made by a worker, was one edit behind.** Found by
the GUI measurement below, which edited a cell of scanner's spreadsheet
and compared every result with a recompute here: each worker's result was
the previous value's, 43 objects off. A sheet's values, one per cell, are
dynamic properties marked `Prop_NoPersist`, and a worker's recorder
leaves out what is not saved. The put-in carried the sheet's `cells`, its
contents, but not its values. Here the values stayed as they were, so
nothing reading them was touched, and the worker's results for those
objects went unused (`Unused` in the info). The next recompute here then
made the sheet, and the next worker job read that value. Fixed two ways:

- `Spreadsheet::Sheet::resultInProperties()` is false: its result is not
  all in saved properties. A put-in makes it here (`madeHere`), and an
  undo of its cells leaves it touched.
- A worker no longer skips an object made here when another object in the
  document reads it (`Recorder::make`). It makes the object for its
  readers and does not send its values. Before, the worker left the sheet
  as it was, and everything reading it was made from the old value.

Test `testAnEditOfASpreadsheetIsMadeFromByAWorker`: an aliased cell read
by an expression, edited twice and recomputed by a worker, then once more
after a recompute here. It fails without the change (`5.5 != 5.6`).
Seen in the code on the way, not run: `PropertySheet::Restore` marks no
cell dirty, and `Sheet::execute()` evaluates only dirty cells and those
in error. So a sheet whose cells are read back live, as an undo past the
hot window reads them, would keep the values of its old contents until
a cell is set. A worker no longer depends on it (the change above).
Whether an undo shows it is not checked.

**A link group kept the shape of the edit before.** The same GUI
measurement, once the sheet was fixed, still found `LinkGroup` and
`LinkGroup001` a value behind after a worker's result. A link group's
shape is made of its members' and kept in its shape cache
(`PropertyShapeCache`). The cache is cleared when the group's own
`_LinkTouched` changes, which its `execute()` touches. A put-in executes
nothing here, and `_LinkTouched` is not a saved value, so the cache stood.
Now Part clears an object's shape cache whenever the object is recomputed
(`signalRecomputedObject`, AppPart.cpp); a put-in reports each object so.
Test `testALinkGroupsShapeFollowsAWorkersResult` (a box in a link group,
edited twice) fails without it, `999.99 != 2000.0`.

`pool.expectedRecompute([objects])` now takes the objects whose recompute
is asked about; with none, what is touched, as before. Without it, each
reading was of the whole touched set, which misled this section's first
look at the times.

**The split on a real edit (item 1 of the user's list).** Scanner in the
GUI (`~/works/sw/fcad-probes/worker/editsplit_gui.py`): the cell behind
`hole_radius`, the alias whose reach includes `PolarPattern003`, edited
from 5.5 to 5.511 mm and back. It reaches 179 objects, and 398 are
recomputed. Each run from the Recompute command to the result in, the
main thread's blocked time by a 10 ms heartbeat; two workers allowed,
warm:

| | result | main thread blocked |
|---|---|---|
| here | 10.1 s | 10.2 s |
| one worker | 13.25 s | 2.1 s (1.4 s of it the put-in) |

It is not split, not even with the threshold at 1 s. The worker's time is 9.15 s and the
expected 9.3 s, so the times are right. What stops it, as I read the
code and the model, not measured cut by cut, is the model's shape. The part holding the longest chain, the pattern's, takes
everything that uses the cut and everything those read. In scanner the
assemblies (`LinkGroup001`, the `App::Part`s above the bodies) use the
pattern's body and read every other part as well, so that part is the
whole reach, and no cut saves anything. An edit above the bodies always
reaches the assembly, so this is likely the usual case, not scanner's
accident. Why sec 42's recompute of every object did split is not
checked. A way through: leave
an object that reads from both parts, when it is cheap (an assembly
gathering shapes), to be made here as the result is put in, and split
what it gathers. Not built; it puts that object's time on the main
thread.

Of the 4.1 s between the worker's 9.15 s and the 13.25 s result, 1.4 s
is the put-in; the rest (the copy's new version, the job's start, the
worker moving to it) is not broken down here.

Again in the GUI with the fixes (0a2435110b): no link group differs any
more. Each run of a worker, split or not, differs from here in
`SectionView007` (19 visible edges here, 20 from a worker) and
`Dimension014` (48.5 here, 90.0 from a worker), the TechDraw kind of sec
43, open. And in one object's last place of four, not the same object
each run (`Part012`'s area 24940.2409 against .2408, `Helix`'s volume),
4e-9 relative.

## 45. A first save while workers read the store (Windows) (2026-11-22)

On Windows (session dwin), three TestWorkerPool tests failed to rename a
file: `testMeshAndPartFilesAreTheOnesMadeHere` (the ExportModules setup),
`testTheFileIsTheOneMadeHere` and
`testAWorkersLaterStringsAreSavedAndReadBack`. dwin's trace in
`FileInfo::renameFile` found the same thing in each. The rename that
fails is the document's transient directory, renamed by a first save
from `Thundereal_Doc_<uid>_da39a3_<pid>` (da39a3, the hash of no file
name) to its file's hash (Document.cpp, the `Uid` branch of
`onChanged`). `MoveFileExW` gives error 5. The Restart Manager names one
holder of `history/log.db` with its `-shm` and `-wal`, and of
`blobs/seg-1.2`: a worker, the one that read the version from this
process's store (sec 3, `ReadStore`). The parent holds nothing there, and
nothing is read-only. Retried every 10 ms, it still failed after 2 s:
the worker keeps them open while it holds its copy. Windows renames no
directory a file under which is open; macOS and Linux do, which is why
only Windows failed.

Now the pool lets go first. `WorkerPool::releaseDirectory(dir)`, called
before the rename, ends each worker whose copy is read from a store
under `dir`: told to quit, and one with no job waited for, up to 5 s. A
worker making a job is told too and ends after it; it is not waited for
inside the document's change, so a save in the middle of a job still
fails to rename on Windows, as before. The copy is marked stale, so the
next job's copy is read from where the store went. Ending the workers
costs a start on the next job, but the copy would have been opened whole
anyway at the new path, and the rename happens only on a document's
first save or a save under another name.

Test `testAFirstSaveEndsTheWorkersReadingTheStore`: a job read from the
store, `saveAs`, then the worker that read it is gone, the copy is stale,
and the next job reads the value edited after the save. Without the call
it fails on macOS too: the worker is still there.

dwin saw a second kind, with no worker: five PartDesign sheet-metal save
tests failed the same rename in one run, one of them in the next, and nothing held
a file by the time the trace looked. With retries, the one that hit it
succeeded on the first retry, after 31 ms: an opener from outside, most
likely the laptop's file monitoring. On Windows the rename is now
retried, up to 20 times, 50 ms apart (first 10 times 25 ms apart; widened
after dwin's retest, where TechDraw's `testOutOfRangeEnumerationIsPutBack`
got through on the fourth retry, about 100 ms in). Each failure still
prints its `Error in renameFile` line. dwin's retest of d3cb4101ca:
TestWorkerPool 74 OK with no rename failure, TestTechDrawApp 25 OK, and
no rename failure in the sheet-metal suites (TestSheetMetalConvert's
`testSolidOutside` fails there on its own account).

The fourth case, TechDraw's `testOutOfRangeEnumerationIsPutBack`, did
not fail in dwin's run.

## 46. TechDraw's dimensions after the migration, here and from a worker (2026-11-22)

Sec 43 left two problems with scanner's dimensions: made here, some
measured another element after the migration; made by a worker, most
came back with other values. Both are fixed. Probes are
`~/works/sw/fcad-probes/td/alldims.py` (every dimension, as opened against
a recompute, here or with `WORKER=1` by a worker) and `views.py` (every
view and every dimension, all objects touched, here against a worker).

**Here.** When a view is made again, a dimension finds its references
again, in rungs: by the name each projected element carries
(`fixByName`), then by the geometry saved with the dimension
(`compareSavedGeometry`, `fixExactMatch`). Three faults:

- **A reference found by name saved the other references' stale
  geometry.** Repointing one reference changes `References2D`, and that
  change saves every reference's geometry and name as it then stands
  (`onChanged`, `updateSavedGeometry`). Dimension264 measures a vertex and
  an edge. Its edge was found by name and moved from `Edge4` to `Edge5`.
  The vertex's `Vertex5` now named another point, which was saved as
  what the vertex meant, so the next comparison passed and the dimension
  measured 4.4245, not 15.9821. Now `repointReferences` repoints all
  found references in one change, then puts back the saved geometry and
  name of each reference it did not repoint, so the geometry rung still
  looks for them. Dimensions 264 to 267 and 274.
- **A reversed edge was another edge.** `GeometryMatcher` compared a
  line, and an arc's ends, in order. OCCT 8's projection runs some edges
  the other way round from 7.7's (Dimension198). Now either way matches.
  An arc must also have the same middle point, since the two halves of a
  circle have each other's ends, reversed.
- **A line written as a B-spline was another edge.** OCCT 8 writes as a
  B-spline some edges 7.7 wrote as lines (Dimension197's edge). TechDraw's
  `isLine` measures the B-spline's whole pole polygon, and a projected
  edge is often a trimmed piece of a longer B-spline, so it said no. Now a
  B-spline against a line, a circle or an ellipse, and a B-spline against
  a B-spline, match when the ends match either way and points a quarter,
  half and three quarters along each lie on the other.

Every scanner dimension recomputed here now keeps the value it had in the
file (145 of 147). The two that do not:

- `Dimension012`: invalid, a distance on an edge that is a B-spline now
  (sec 43).
- `Dimension075`: its saved geometry was stale in the file itself. The
  file's value, 7.2427, is the distance between the vertices the view has
  now, (17.5, 5.757) and (11.049, 13.0). Its saved points are (17.5,
  7.307) and (11.049, 13.75), which are not in the view, and its saved
  names carry ids from before the migration. Nothing in the file finds
  those two vertices again.

**From a worker.** Three faults stopped what the worker made from coming
back:

- **Saved geometry restored null.** A version of the document, which a
  worker reads, names each file of a property by its content as well:
  `Dimension264.SavedGeometry.0.4ae0069a9881.brp`.
  `PropertyTopoShapeList::RestoreDocFile` read the entry's index from the
  last part before `.brp`, so `atoi("4ae0069a9881")` gave 4, out of
  range, and the entry stayed null. Most scanner dimensions came into the
  worker with nothing saved to find their references by. Now each file is
  the entry `Restore` registered it for, looked up by its base name (a
  directory project hands the reader the folder too).
- **A null entry threw.** A transaction copies the list before a change,
  and `PropertyTopoShapeList::Copy` ran `BRepBuilderAPI_Copy` on a null
  shape, which throws `Standard_NullObject`. A worker has a transaction
  open, so its dimensions failed there with an exception whose message
  was empty: `exception in scanner#Dimension264 thrown:`. A null entry
  is now copied as null. The dimension's ladder also catches OCCT
  failures now and reports them with their type
  (`... (Vertex5, Edge5): Standard_NullObject`), instead of letting them
  out of `execute()`.
- **What a view's execute recomputed itself never came back.** A
  projection group's execute recomputes its views, and a view's execute
  recomputes its dimensions (`postFaceExtractionTasks`). The worker
  records each of those as an entry of its own (sec 19). Here the group
  and the views are put in, not executed, and scanner's dimensions are
  not touched, so this process's recompute never reached them ("Skip
  recomputing"): `Unused: 104` entries in the put-in report. Now the
  worker writes, for each entry, the entry whose execute ran it (format
  `FCWRCP02`). When that outer object is put in, each inner one is
  recomputed in turn, with `enforceRecompute()` as the view asks for it,
  and is filled from its own entry. `recomputeInfo()` counts them in
  `Inner`. Entries for objects this process makes itself are dropped
  before the file is written (a spreadsheet the model reads, sec 44), and
  the outer places are renumbered for what is left. Without that the
  file did not read: "its objects do not read".

With the three, a worker's recompute of scanner (16 s) matches a
recompute here: the same dimensions differ from the file, 012 and 075.
With every object touched, every view (35) and every dimension (147) is
the same from a worker as made here, with 835 objects supplied, none
executed here and none unused, in the console. Sec 44's two differences
(SectionView007, Dimension014) were seen in the GUI, which was not run
again for this; in the console neither differs any more.

Tests:

- `TDTest.DimensionReferenceTest` (TestTechDrawApp), each case built by
  hand on a box's view. Each fails without its fix:
  - `testAReversedEdgeIsFoundAgain` and `testALineSavedAsABSplineIsFoundAgain`
    fail without the matcher.
  - `testAReferenceFoundByNameKeepsTheOthersSaved` fails without
    `repointReferences` keeping the others.
  - `testTheHalvesOfACircleAreToldApart` fails without the middle-point
    check.
  - `testSavedGeometryWithANullEntryIsCopied` fails without the null copy.
  - `testSavedGeometryInFilesNamedByContentIsRead` fails without the
    restore by file. It renames a saved project's files to
    `<index>.<hash>` names that parse wrong.
- `TestWorkerRecompute.testAViewsDimensionsComeBackWithIt`: a projection
  group's view swaps its two sources, so the projection numbers the same
  outline otherwise, and the dimension comes back from the worker on its
  elements' new numbers. It fails without renumbering the outer places
  (its spreadsheet's entry is dropped). It **passes without the replay of
  inner objects**: in this small document the dimension is touched and
  recomputed in the normal order. What leaves scanner's dimensions
  untouched was not found, so scanner (`Unused` 104 before, 0 after, and
  the values above) is the evidence for the replay.

Also fixed on the way: `DrawStoredGeometryTest.testASilhouetteIsNamedFromItsFaceAndSurvivesAReload`
(TestTechDrawGui) looked for a source named `Face1`. A cylinder's side has
been named `Lateral` since a022ec951f (2026-10-09), and the test had
failed since then.

Open:

- A dimension made in the same recompute as its view saves its references
  before the view has drawn anything, so it saves null geometry. Nothing
  fills those entries in later, so its geometry rung can never find them;
  each recompute says "no exact match". In the GUI a dimension is made
  after its view is drawn.
- `handleNoExactMatch` accepts a reference that was not found as it stands,
  so a dimension like Dimension075 measures another element with no error.

## 47. A rename or a delete waits out a hold for a moment (2026-11-22)

Sec 45's second kind, a rename refused while nothing named held the file,
is not Windows' own. dwin runs on a company laptop under IT monitoring,
whose file-system filter opens a file a moment after it is written. An
antivirus, an indexer or a slow network share does the same anywhere.
How long such a hold lasts depends on the box, 31 ms and 100 ms on
dwin's. So a fixed retry around one call site was the wrong shape. Now
`Base::FileInfo` waits such holds out for every rename and delete
(`renameFile`, `deleteFile`, `deleteDirectory`, and so
`deleteDirectoryRecursive`), and learns how long to wait:

- **Momentary refusals only.** On Windows these are a sharing or lock
  violation, a mapped file, access denied unless the file is read-only,
  and a directory not empty (a file deleted while something held it
  still lists). Elsewhere they are `EBUSY` and `ETXTBSY`. A directory not
  empty there stays so: POSIX has no delete pending. Any other refusal
  fails at once, as before.
- **Waits** double from 2 ms to 100 ms between attempts.
- **The budget is learned.** It is 1 s until a hold has been seen, then
  four times the longest hold that cleared in this process, never more
  than `FileRetrySeconds` (User parameter:BaseApp/Preferences/General,
  10 s by default; 0 tries once). A slow box widens its own budget after
  its first slow hold; a fast one keeps the second.
- **Logging.** An operation that went through after retries is logged
  once ("rename of '...' went through after 4 tries, 104 ms"). One still
  refused is reported once, with its tries and time. The `Error in
  renameFile` line for every attempt is gone.
- `FileInfo::retryStats()` counts them: retried, recovered, failed, the
  longest hold, the current budget.

Document's own retry around the transient directory's rename (sec 45) is
gone with it. Ending the workers that read the store stays: that hold is
ours, and lasts as long as they do.

Not told apart: a lasting hold by another program (an editor with the
file open) waits the whole budget before it fails. On Windows the Restart
Manager could name such a holder, but it does not see a filter driver,
and a named holder is the rarer case.

**dwin's retest (947618049e).** It builds on Windows. FileRetryTest passes
6/6, TestWorkerPool 75 OK, TestTechDrawApp 31 OK, sheet metal as before.
One rename went through after 7 tries, 185 ms. But in 7 worker tests a
blob segment's delete was still refused after the full 1 s (error 32, a
sharing violation). dwin's trace at the give-up:
- one holder each time, a worker (a child ThunderealCmd of this process);
- the file not delete-pending, a few kB, gone 2 s later;
- this process holding only `history/log.db` and its `-wal`/`-shm`.

So it was a lasting hold of ours, not a monitor's. The store's
maintenance rewrites a segment as a newer generation and deletes the old
one, which a worker reading the version still has open. `BlobArchive` read
through a C++ stream, and MSVC's opens without `FILE_SHARE_DELETE`, so
the delete waited until the worker let go. POSIX lets the delete through,
and the worker reads on from the unlinked file. A segment is now read
through `SharedReadFile`. On Windows it is `CreateFileW` with read, write
and delete sharing, read at offsets with `ReadFile`. On POSIX it is
`open` and `pread`. A delete or a rename of a segment no longer waits on
its readers. Not run on Windows yet.

Tests (`tests/src/Base/FileRetry.cpp`): the test hook
`FileInfo::setRetryFault` refuses attempts as each case needs.
`aMomentaryHoldIsWaitedOut`, `anotherRefusalFailsAtOnce` (under 50 ms),
`aLastingHoldFailsAtTheLimit` (0.2 s), `aZeroLimitTriesOnce`,
`theWaitGrowsWithTheLongestHold` (1 s, then four times a 0.3 s hold,
then capped by the limit), `aDeleteIsWaitedOutToo`. They run on macOS;
the Windows branch is not compiled here.

**dwin's retest (070e9a8a96).** It builds on Windows. FileRetryTest 6/6;
TestWorkerPool 75 OK with Log on and no "still refused" (r5 and r6 had 7);
FileBlobs 97 OK, Document 363 OK. One rename went through after 7 tries,
197 ms: the first save in `testAViewsDimensionsComeBackWithIt`, as in r6.

## 48. A killed worker's folders are removed (2026-11-22)

dwin's test homes kept 96 `Thundereal_Doc_*` and 90 `Thundereal_Reader_*`
folders over three runs, never deleted. This box's cache had 8089 and
4770, 2.4 GB, of which 2799 and 1411 were not empty. A worker's copy of
the document and its reader of this process's store (sec 3) each have a
transient folder in the user cache, named after the worker's pid. A
worker that quits removes them as it closes. The pool kills its workers
when it stops or its document closes, so they were left.

Now the pool removes `<exe>_Doc_*_<pid>` and `<exe>_Reader_*_<pid>` of
each worker once its process has ended: in `finished()`, in `stop()` and
in the destructor, each after the process is reaped. Test
`testAKilledWorkerLeavesNothingInTheCache` (stopped, and by a close)
fails without it. A whole TestWorkerPool run now leaves one empty
document folder in the cache, not of a worker.

Not done: the folders already left, and those other processes leave when
killed or crashed. Which of this box's 2.4 GB were workers' is not known:
a pid no longer says what it was. A document's folder may hold the transaction log a
recovery reads (`Application::recoverDocument`), so removing those is a
decision about recovery, not a cleanup. Asked of the user; sec 49 is the
answer.

## 49. The start removes what ended processes left that holds nothing (2026-11-23)

The user's ruling on sec 48: at start, remove the folders of processes
that no longer run that cannot hold anything to recover. Those are a
store's reader folders, and document folders that are empty. A document
folder with anything in it stays, whatever is in it.

`App::CacheSweep::start()`, at the end of `Application::initApplication()`,
sweeps the user cache on the shared thread pool. It runs for the console
and the GUI alike, but not in a worker (its env has
`THUNDEREAL_WORKER_SERVER`): sec 48 removes a worker's folders, and a pool
starts many workers. `run()` looks at `<exe>_Reader_*` and `<exe>_Doc_*`, and
takes the pid from the end of the name. It leaves a folder alone if any of
these holds:
- the name ends in no pid, or in this process's;
- the folder was changed after the sweep began, so one a process makes
  meanwhile is never touched;
- the process may still run.

A process may run when it is there and its start time is not known: Linux,
and a Windows process this user may not ask. Pids are reused. Where the
start time is known (macOS through sysctl, Windows through
`GetProcessTimes`), a process that started more than a second after the
folder last changed did not make it. Then a reader folder is removed whole,
and an empty document folder is removed. The sweep stops at the next
folder once the application shuts down (`isShuttingDown()`, or Qt's
`closingDown()`), and logs one line of what it did.

The GUI's start already had a scan of its own (`DocumentRecoveryFinder`,
in `Gui/DocumentRecovery.cpp`). It offers the recoverable document folders
of dead sessions and removes the others. It never ran for the console,
which is what the tests and the workers start, and it does not look at
reader folders. It is left as it was.

On this box, the first console start after the change took the cache
from 8091 document and 4771 reader folders, 2.4 GB, to 2800 and 1, 1.9 GB.
That is 5291 empty document folders and 4770 reader folders removed. The
two it kept were pid 39432's, a pid a system process had reused (started
Nov 23, the folders last changed Nov 21). That was before the start-time
check, which went in for them; the next start removed both ("Removed 1
reader and 1 empty document folders of ended processes from the cache, 0
would not go"). Of the 2799 document folders that remain, 2798 hold a
`history/log.db`, which the GUI's recovery offers. The other holds only a
`blobs` folder, and the GUI's scan would remove it as not recoverable.
There are also 13 lock files, which the GUI's scan removes once none of
their pid's folders are left.

Test `tests/src/App/CacheSweep.cpp` (7 cases) runs `run()` on a folder of
its own. It covers an ended process's reader and empty document, a
document with something in it, a running process's folders and this
process's, a folder changed since the start, other programs' and other
names, a stopped sweep, and a reused pid. The last case expects the folder
gone on macOS and kept elsewhere.

## 50. Why the folders the sweep keeps were left (2026-11-23)

The user asked, of the 2799 document folders sec 49 kept: are they known
leftovers? Find out why they were left first, then clear them. Each of
them holds a `history/log.db`, so the GUI's recovery would offer every one.

On this box, the suites run one after another left six folders in the
cache. Document left five, from three processes, and TestWorkerPool one
(both 2026-11-23). Run class by class, and then test by test, the folders
came from three causes.

1. **`sys.exit()` from the console skipped the documents' close.**
   `FreeCADTest.py`, what `-t` runs, ends with `sys.exit(...)`.
   `MainCmd.cpp` answered `SystemExitException` with `exit(code)`, which
   skips the destruction phase: `closeAllDocuments()` and
   `Application::destruct()`. Each document a test left open kept its
   folder, with its log. A script that ends without `sys.exit()` closed
   them; one that calls it did not. Shown with a one-test module that
   leaves one document open: under `-t` its folder stayed, and no longer
   does. Of Document's tests, `XLinkIntoUnsavedCases.testACopyIntoADocumentNeverSaved`
   reopens `dst.FCStd`, and the link opens `bolt.FCStd` with it, both named
   after their files. The tearDown closed `XUnsaved*` only, so two folders
   were left per run.
   Now a `sys.exit()` falls through to the destruction phase, and `main`
   returns its code. A run with a test that fails still exits 1, and
   `sys.exit(7)` exits 7. The tearDown also closes what was opened from
   the test's folder. The GUI's `main` (`MainGui.cpp`) answers the same
   way and is left: its documents close with its window, and closing them
   after the window has gone is what the GUI probes must not do.

2. **A pinned version an undo opened again never named its folder.** In
   `TransactionBranchCases.testReleasedPinnedVersionClosesHeadless`, an
   unpin closes the version (`ClosePinnedVersion` 1), and the undo of the
   unpin opens it again, as the link resolves. While an undo is applied,
   the property changes are held (`Transaction::isApplying`) and replayed
   after it. So the new document's `Uid` reached `onChanged` only once
   `_openVersionDocument` had made it a `FrozenVersion`. The Uid branch
   made the folder, and its `TransientDir.setValue` went through
   `onBeforeChange` to `checkNotFrozen`, which threw. The held changes'
   replay swallowed the throw, so nothing said so. The document lived with
   `TransientDir` empty, and its close removed nothing. Traced with prints
   in the Uid branch: the folder was made, and the write after it never
   returned. On the first open the same branch ran inside the restore,
   which `checkNotFrozen` allows.
   Now `Document::setTransientDir()` writes it as bookkeeping
   (`d->bookkeeping`, the flag `_openVersionDocument` names the version's
   label under), for both the made and the renamed folder. The test now
   asserts that no folder named for the part's Uid is left at its end. It
   fails without the fix ("Lists differ: ['Thundereal_Doc_d57e48ba-..._17747']
   != []") and passes with it.

3. **Two tests end a child with `os._exit()`, and left its folder.**
   `UndoRedoCases.testProcessEndedFromAScriptEnds` must end its child with
   a document open: that is the case it tests. Its child now prints the
   folder, and the test removes it. In
   `TransactionTwoProcessCases.testAFolderSavedToItsHistoryElsewhereIsCopiedWhole`,
   `os._exit()` only skips the helper's save, so the child now closes its
   document first.

A wrong turn, for the record: a bisect of the class from a script named
`grp.py` found nothing, twice. A `.py` on the command line is loaded as
a module by its name, and `grp` is a module of Python's own, so none of
those runs ran a test.

After the three: Document (363), TestWorkerPool (76), FileBlobs (97),
TestTechDrawApp (31) and TestPartApp (446) each pass with exit 0 and leave
nothing in the cache, measured by listing it before and after each. ctest
passes all 1295 that ran, none failed.

The old folders. By day: 273 on Nov 12 from 21 processes, about 13 a
process, which looks like what cause 1 leaves; 1597 on Nov 16 from 1006
processes, one or two each, which looks like killed workers (sec 48). That
is a reading of the counts, not a trace: their reader folders were swept
already, and a pid says nothing now. Every one of them is of a process no
longer running.

`-t 0` was run too, and it aborts in
`CAMTests.TestPathToolShapeIcon.TestToolBitShapeSvgIcon.test_get_qpixmap`
(Qt's font database without a QGuiApplication; known since sec 29, not
chased). That aborted run left 14 folders itself, which is a crash's
leftover as designed.


## 51. A GUI test run ended in an abort, and its lock stayed (2026-11-23)

After sec 50 the cache held 13 lock files, `Thundereal_<pid>.lock`, all
of dead processes, the oldest from Nov 14. The GUI writes one per session
and removes the dead ones only from `checkForPreviousCrashes`, at the end
of `MainWindow::delayedStartup`. A test run returns before that, and so
does a script on the command line that ends the program. No session on
the mini got that far after Nov 14, so nothing removed them.

The three newest (pids 87808, 87952, 629, Nov 22) each have a crash log
from the same second:
`libc++abi: terminating due to uncaught exception of type
Base::SystemExitException`, then SIGABRT. The stack runs through the
`delayedStartup` lambda that runs a `-t` test. That run ends with
`sys.exit(0 if result else 1)`. The lambda caught the exception and threw
it on, through Qt's timer frames, where it ends the program, though
`GUIApplication::notify` is further out and catches it. `build/td-gui.log`
from then shows `Ran 22 tests ... OK` followed by the abort. So every
`Thundereal -t` run here ended with SIGABRT, whatever its result. The
other 38 crash logs are the CAUGHT reports of the worker-crash tests,
which is expected.

Two fixes:

1. `GUIApplication::exitWith()` does what `notify` does with a caught
   `sys.exit()`: it keeps the exception for `runApplication`, which throws
   it once the loop has ended, and it ends the loop with its code. The
   test-run lambda and the command-line files of `delayedStartup` call it
   and no longer throw on. The latter returns, as the program is ending.
2. `runApplication` threw the kept exception before it removed the lock,
   so a GUI session that ended through `sys.exit()` left its lock behind,
   even without a crash. The lock now goes first.

Measured after the build: `Thundereal -t TestTechDrawGui` offscreen ran 22
tests OK, exit 0. `Thundereal -t NoSuchTestModule51` failed its import,
exit 1. Neither wrote a crash log, and the cache was empty after each.
The 13 locks were removed by hand beforehand. Each had a dead pid and no
folder of its own.

## 52. A full `-t 0` runs to its end (2026-11-23)

Sec 50 left `-t 0` aborting in
`CAMTests.TestPathToolShapeIcon.TestToolBitShapeSvgIcon.test_get_qpixmap`,
with "QFontDatabase: Must construct a QGuiApplication before accessing
QFontDatabase". That module, run alone, passes with 32 run and 6 skipped.
Its guard is `QtGui.QApplication.instance()`, which is None in a console,
so the painting tests skip. But the worker pool's `ensureCore()`
(`App/WorkerPool.cpp`) makes a `QCoreApplication` for its sockets, so
after the first worker test of a `-t 0` the guard is not None. The test
then paints without a `QGuiApplication`, and Qt aborts.

To reproduce, a script makes `QtCore.QCoreApplication([])` and then runs
the module: exit 134, the same message. The guard now asks for an
instance that is a `QGuiApplication`. After the fix the same script runs
32 tests, 0 failures, 6 skipped, exit 0.

Two other places ask `QApplication.instance()` outside a GUI module:
CAM's `AssetManager.get` (a thread warning) and the AddonManager's quit
hook. Both are right with a `QCoreApplication`, so neither was changed.

The whole of `-t 0` after it: 5557 tests in 785 s, OK (skipped=178,
expected failures=6), exit 0. It wrote one crash log, the CAUGHT report
of the worker-crash test, and the cache was empty after it.

## 53. Where a worker's recompute spends its time; the previews nobody draws (2026-11-23)

Sec 44 left 4.1 s between the worker's 9.15 s and the 13.25 s at which
scanner's `hole_radius` edit came back in the GUI: 1.4 s for the put-in,
the rest "not broken down". It is broken down now. Each recompute put in
records where its time went, on this process's clock, in
`recomputeInfo()["Times"]`:

- `Start`: inside `WorkerRecompute::start()`, mostly the copy's new
  version (`WorkerPool::start()`, a snapshot to the log).
- `Queued`: from the job's submit until it is handed to its worker.
  `Setup` is the part of that the worker spent opening or moving its copy.
- `Job`: the worker's handler, as it says itself.
- `Returned`: from the hand-over until the result is read here.
- `Held`: from the result read until its put-in starts.
- `Read`: reading the result's file and tables.
- `Apply`: the put-in (`ApplySeconds`).
- `Total`: from `start()` until the put-in is over.

The pool's `Result` carries `queued`, `returned` and `setup`, for any kind
of job. A split recompute reports the part that came last.
`TestWorkerPool.testAPartChainMadeByAWorker` checks that the parts add up
to `Total`.

Scanner in the GUI (`~/works/sw/fcad-probes/worker/times_gui.py`, sec
44's probe with one worker, no split, the timeline printed; one warm-up,
two recomputes here, then three by the worker):

| run | Start | Queued (Setup) | Job | Returned - Job | Read | Apply | Total |
|---|---|---|---|---|---|---|---|
| after two here | 0.58 | 1.36 (1.17) | 9.87 | 0.004 | 0.07 | 1.35 | 13.24 |
| warm | 0.64 | 0.06 (0.03) | 9.81 | 0.002 | 0.09 | 1.13 | 11.73 |
| warm | 0.61 | 0.04 (0.03) | 9.88 | 0.004 | 0.09 | 1.09 | 11.70 |

The parts add up to `Total` to the millisecond, and `Total` to the
probe's own measure within 13 ms. So:

- **Most of sec 44's unexplained time was a new worker opening the copy:
  1.17 s.** I first read it as the worker moving across the rows the two
  recomputes made here wrote. Sec 57 found it was not: the worker had
  quit during those recomputes (`IdleSeconds`, 30 s), and the job started
  a new one, which opens the version whole (`Spawned` 2, `Moves` and
  `Reopens` 0). After its own result a worker moves in 0.03 s, because it
  keeps what it sent (sec 29).
- **Handing the result over takes nothing**: `Returned` is `Job` plus a
  few milliseconds.
- **Outside its recompute** the worker spends about 0.65 s (`Job` 9.8 s
  against `WorkerSeconds` 9.15 s).
- **On the main thread**: `Start` about 0.6 s and `Apply` 1.1 to 1.35 s.

**The put-in meshed every PartDesign preview.** A `sample` of the main
thread during a warm run (14 s at 1 ms) put 674 of the put-in's 982
samples in view updates. 341 of those were
`ViewProviderAddSub::updateAddSubShapeIndicator`, which rebuilds the
feature's AddSubShape preview at every change of that shape:
`BRepMesh_IncrementalMesh` on the main thread, for a preview that is down.
The helix alone was 263 samples, its preview shape a long sweep. The
preview's view is made with `forceUpdate()` and shown, so the hidden-object
rule of sec 16 never applied to it. A recompute made here paid this too.

Now `updateAddSubShapeIndicator` builds only while the preview is up
(`previewShown()`: `previewActive`, the display mode, or a session's
preview). Otherwise it leaves the visual touched
(`ViewProviderPartExt::touchVisual()`). `setPreviewDisplayMode(true)` and
a detail path into the preview build it first (`ensureVisual()`). The
bounding-box build of sec 16 covers any other traversal that reaches it.
Colours live in the material node, not the mesh, so a preview built later
shows the colours set while it waited. The overrides that build
differently are left as they are: Wrap shows another object's view, and
DressUp and Defeaturing compute their preview shape.

After it, the same probe:

| | before | after |
|---|---|---|
| here | 10.2 s | 9.73 s |
| one worker, warm | 11.7 s | 11.2 s |
| `Apply` | 1.09 to 1.35 s | 0.63 to 0.88 s |
| main thread blocked | 1.8 to 2.1 s | 1.35 to 1.72 s |

GUI tests of the preview: pd-two-edits-preview 22, handle-over-preview 5,
fillet-preview-pick-syncs-list 23, serve-pd-preview 32,
serve-pd-multi-user-preview 15, serve-dressup-preview-pick 11 and
serve-pd-multi-user-no-preview 14 pass every claim.
`fillet-groups-and-preview-menu` passes 12 and then hangs at its colour
dialog until the runner's deadline. It does the same with the old
`ViewProviderAddSub.cpp`, so that is not this change. pd-preview-per-view,
pd-edit-shown-per-view and edit-on-top-by-preview-and-path fail as they
did before on the mini (see the handoff's list). TestWorkerPool 76 OK.

`Start` is left as it is. The snapshot costs 0.30 s with no shape changed
(console, an unread cell set). Every shape's `makeBlob` runs the sharing
analysis (`ShapeRefSet::build`), and that is what tells later objects in
the same save what they may borrow. Skipping it for an unchanged shape is
a change to the shared storage's design, which sec 22 already declined.
The 179 shapes the edit changes add about 0.5 s more, and those are new
shapes that have to be written.

**The worker's time outside its recompute is the view's meshes.** In the
console a warm run's `Job` is 0.07 to 0.08 s longer than `WorkerSeconds`
(`~/works/sw/fcad-probes/worker/workersample.py`). In the GUI it is
0.65 s longer, and the GUI's job carries display hints (sec 14). The same
GUI probe with `Workers/RecomputeMeshes` off: `Job` 9.26 to 9.29 s against
`WorkerSeconds` 9.19 to 9.21 s, and `Apply` 1.26 to 1.30 s against 0.63
to 0.69 s with meshes. So the 0.6 s is meshing the worker takes off the
main thread, and it stays. Meshing each object as the recompute goes, on
another thread, would overlap it with the recompute. But it writes
triangulations into shapes the later features are still reading, which
sec 20 showed a result can depend on, so it is not done.

What is left of the gap is a new worker's open of the copy (1.15 s, in
the worker, when the last one quit while the user worked here; sec 57
corrects this from "the move") and `Start`'s snapshot (0.6 s, on the main
thread).

## 54. A dimension made with its view saves what it measures (2026-11-23)

Sec 46 left this open: a dimension made in the same recompute as its view
saves its references' geometry before the view has drawn anything.
`updateSavedGeometry()` runs when the references are set, and a reference
whose element is not there yet gets a null entry. Nothing filled those
entries later. At every recompute `compareSavedGeometry()` found the null
entry's type unlike the element's, `fixExactMatch()` found nothing like a
null shape ("Dim - no exact match for changed 2d reference: 0", one line
per reference), and `handleNoExactMatch()` accepted the reference as it
stood. The value was right, but the reference had no geometry to be found
again by when its view numbered the elements otherwise. A script makes
dimensions this way, and so does a worker's copy.

Now `execute()` fills a null entry from what its reference names, once
the reference has geometry (`fillNullSavedGeometry()`), before the
comparison. That is the geometry it was made against: there was nothing
else. The entries stand for the references that have a subname, the same
correspondence `updateSavedGeometry()` makes. A restore fills nothing.

`DimensionReferenceTest.testADimensionMadeWithItsViewSavesWhatItMeasures`:
a box, its view and a dimension added and recomputed once. Before the
recompute both entries are null, which checks the case is the one meant.
After it, neither is, and they hold the same points, and give the same
value, as a dimension made after its view. Without the fix it fails ("True
is not false"), with the two "no exact match" lines. With it,
DimensionReferenceTest 7 OK, TestTechDrawApp 32 OK and TestWorkerPool 76
OK. TestTechDrawApp still prints "no exact match" twice per run of
`testDimensionMeasuresWhatTheUndoPutBack`, which changes the model under
its dimension: that is the message working.

`handleNoExactMatch()` still accepts what it does not find, unchanged. It
is also what keeps a dimension on an edge the user lengthened, which
matches no saved geometry exactly, so an error there would break the
ordinary edit. Dimension075 (sec 46) is the other side of it: saved
geometry that is stale in the file.

## 55. A save keeps the analysis of a shape that has not changed (2026-11-23)

Sec 53 left `Start`'s floor alone: 0.30 s of each snapshot of scanner
with no shape changed, nearly all of it `ShapeRefSet::build()`, which
every shape's `makeBlob()` runs in every save. I had called caching it a
change to the shared storage's design. The user did not see why, and on a
closer look it is not one: a cache, with a check that it still holds.

What the analysis is for, and why "skip it if the shape is unchanged"
alone would be wrong:

- It decides what the file borrows from files written before it in the
  same save (its plan, by their content hashes), and
- it publishes what the file holds into this save's owner table
  (`ShapeRefSet::publish()`), which is what lets later files borrow from
  it. The table is made afresh for each save and numbers its files in save
  order.

Skipped outright, later files would borrow nothing from it. And a kept
plan could name a file this save no longer writes.

So each `PropertyPartShape` keeps the last save's `ShapeRefSet`
(`_analysis`), with the shape it was made of, its options (pcurve dedup,
stable bytes, geometry sharing, below-face level), and the hashes of the
files it borrowed from. A save takes it instead of walking when all of
these hold:

- the shape is the same (location stripped, so a moved object keeps it,
  as it keeps its file);
- the options are the same;
- the property's file is still held, and the plan comes out as the plan
  that file was written with, so `write()` never runs on a kept set;
- every file it borrowed from has been written again in this save.
  `ShapeRefSet::rebind()` finds each one by its hash
  (`ShapeOwnerTable::fileOf()`) and renumbers `_borrowed`, the only place
  a set holds owner-table numbers. Everything else names a slot in it, and
  what `build()` plans with and does not keep (`_prefer`) is not needed
  afterwards.

Then `publish()` offers the kept set's shapes and geometry to the files
after it, as a walk would. It is dropped with the shape (`dropBlob`), like
the congruence invariants of sec 22, so it pins nothing the property does
not hold. `Mod/Part/KeepSaveAnalysis` (default true) turns it off, and
`Part.saveAnalysisWork()` counts shapes walked and kept.

A kept set keeps what it borrowed. If an object before it changed and now
holds what it holds, a walk would borrow that too. That case only shares
less, and the file is the same either way. On scanner it does not arise:
see below.

Measured, console (`~/works/sw/fcad-probes/worker/analysiscache.py`):

| snapshot | walked | kept |
|---|---|---|
| no shape changed (an unread cell) | 0.30-0.32 s | 0.10-0.11 s |
| the `hole_radius` edit, 179 shapes changed | 0.76-0.81 s | 0.64-0.78 s |

The first save with it on walks everything (0.30 s), and every later one
is 0.10 s. After the edit and back, a save with the analyses kept and one
that walked every shape were compared member by member: all 622 shape
files are byte-identical. `blobs/History.db` differs (the save history)
and `blobs/Content.xml` only in that file's hash. The save with kept
analyses reopens with every one of 405 shapes as the live one, except two
infinite planes whose "volume" is noise (-5e167 against 0.0). The walked
save reopens with the same noise.

`ShapeStorage.ShapeRefCases.testAKeptAnalysisWritesWhatAWalkWrites`: a
borrower beside its source, both unchanged while a third object changes.
The second save keeps 2 and walks 1 (counted), and its three shape files
are those of a save that walks all. It reopens with the borrower's solid
still the source's. `testABorrowerIsRewrittenWhenWhatItBorrowedIsGone`
covers the other side: the source replaced, the borrower unchanged, so its
kept set cannot be rebound and it is walked and rewritten. ShapeStorage
67, FileBlobs 97, Document 363, TestPartApp 446, TestWorkerPool 76 and
TestTechDrawApp 32 are OK. ctest passes all 1285 that run (11 disabled).

## 56. What crashed a worker is trusted again after clean runs (2026-11-23)

Sec 38 left it open that *risky* never expires. An object that once crashed
a worker, and was changed so it is tried again (sec 34), had every later
recompute reaching it handed to a worker (sec 37), for as long as the
document was open, however often a worker made it without a crash since.
The user agreed it should expire.

Now `apply()` counts, for each risky object a worker supplied in a result
that was put in, the recomputes since its last crash. At
`Workers/RiskyClearAfter` (default 3; 0 keeps it risky for good) it is
risky no more, and a recompute reaching it is made where any other would
be. A crash again starts over: `quarantine()` drops the count with the
refusal it puts back. A deleted object drops it too.
`recomputeInfo()["Risky"]` lists the risky objects.

`TestWorkerRecompute.testWhatCrashedIsTrustedAgainAfterCleanRuns`: a
crash, the crash fixed, then three recomputes, each handed to a worker
with `Risky` still `["Crash"]`. After the third, `Risky` is `[]`, and the
fourth is made here (`MadeBy` this process, `HandedOver` unchanged).
Run with `RiskyClearAfter` 0, it fails where it should (`Risky` still
`["Crash"]`).

## 57. A quiet document's workers move before the next job (2026-11-23)

The user's item 1. A recompute made here leaves the workers' copy behind
(`Stale`). The next job then pays the copy's new version (`Start`, on the
main thread) and the worker's catching up (`Setup`), one after the other,
before its recompute starts.

Now a change that makes the copy stale, while a worker is up, starts a
timer of `Workers/MoveWhenIdleSeconds` (default 2 s; 0 is off), and each
further change starts it again. When it fires on a quiet document, the
newer copy is made and every worker is told to move to it at once
(`takeCopy()`, as for a job). Quiet means:

- no recompute, restore or undo running, and no transaction open once an
  implicit one is committed, as `takeCopy()` commits it;
- no job queued or running;
- a worker still up.

Otherwise it waits another delay, or gives up when no worker is left. A
job that comes later finds the version unchanged (`ReuseVersion`) and its
worker there or on its way. It needs an event loop, so a console script
does not see it. `pool.info()["IdleCopies"]` counts the copies made so.

`TestWorkerRecompute.testAQuietDocumentsWorkersMoveBeforeTheNextJob`
(0.3 s, events pumped by the test): nothing is made while a transaction
is open. Once it is committed, one copy is made and the worker moves
(`Moves` +1). Nothing more is made while the copy is current, and the next
job is made on it, with the right result.

**What it buys, scanner in the GUI** (`times_gui.py`, `PROBE_ONEHERE=1
PROBE_QUIET=3`: a worker job, then three rounds of one `hole_radius`
recompute here, 3 s quiet, and a worker job):

| | `Start` | `Setup` | main thread blocked | total |
|---|---|---|---|---|
| off (`PROBE_NOIDLE`) | 0.44-0.47 s | 1.14-1.15 s | 1.51-1.55 s | 12.55-12.71 s |
| on, rounds 2 and 3 | 0.14-0.16 s | 3.93-3.95 s | 0.98 s | 11.97-11.98 s |

The gain is the snapshot made while the user is quiet: 0.3 s less
`Start`, 0.55 s less of the main thread blocked, and 0.6 s sooner in all.
The move itself is no gain.

- **Off, the worker never moved: it had gone.** The probe's 3 s quiet
  follows a 9.7 s recompute and the probe's own reading of every shape,
  and the worker quits after `IdleSeconds` (30 s). So every job started a
  new worker, which opened the version whole in 1.14 s. That is also what
  sec 53's 1.17 s was (corrected there).
- **On, the worker moved, and moving across one recompute made here took
  it 3.9 s**: slower than a new worker's open of the whole version
  (1.15 s). The move started in the quiet time, and the job still waited
  about 1.2 s for the rest of it (`Queued`).

Why a move across those rows costs more than an open is not chased here.
Until it is, a large change is caught up with faster by opening whole than
by moving. That is the next item.

## 58. A worker's move along the log checks no shape again (2026-11-23)

Sec 57 found a live worker moving across one recompute made here took
3.9 s, against 1.15 s for a new worker opening the whole version.
Reproduced in the console (`~/works/sw/fcad-probes/worker/movecost.py`:
`IdleSeconds` 600 so the worker stays, a job, a recompute here, a job):
`Setup` 4.15 s and 3.92 s, against 1.04 s for the first open. A `sample`
of the worker caught part of the move: of 335 samples under
`_moveAlongLog`, 235 were `PropertyPartShape::validateShape()`, a full
`BRepCheck_Analyzer` of each shape restored.

`validateShape()` skips a value an undo or a redo puts back
(`isPerformingTransaction()`): it was checked when it was made, and its
`InvalidShape` comes back with it. Document's own moves along the log, a
branch switch or a recovery, set `replaying`, which
`isPerformingTransaction()` includes (sec 27.68 of
docs/TransactionLogJournal.md: object code must not react to rows put
back). The worker's `moveAlong` called `_moveAlongLog()` directly, with
nothing set. So every shape the recompute here had made was checked again
in the worker. A `FixShape` would also have fixed it there, into something
other than the value the row has.

Not `replaying` for the worker: under it a value the log lacks touches its
object instead of refusing the move. A refused move has the worker open
the version whole, which is right, and a touched object in its copy would
feed stale input to the next job. So the move has its own state:
`Document::moveAlongLogQuietly()` runs `_moveAlongLog()` with
`isMovingAlongLog()` true, and `validateShape()` skips while it is.

Measured, the same probe:

| move across one recompute made here | before | after |
|---|---|---|
| `Setup` | 4.15 s, 3.92 s | 0.44 s, 0.41 s |
| the job, in all | 14.76 s, 14.02 s | 11.03 s, 10.43 s |

So a move is cheaper than an open again (1.04 s), and the pool needs no
rule for opening whole instead. In the GUI, sec 57's probe (one recompute
here, 3 s quiet, a job; rounds 2 and 3, the worker still up):

| | `Start` | `Queued` (`Setup`) | main thread blocked | total |
|---|---|---|---|---|
| quiet move off (sec 57) | 0.44-0.47 s | 1.34-1.40 s (1.14 s) | 1.51-1.55 s | 12.55-12.71 s |
| on, before this | 0.14-0.16 s | 1.2 s (3.9 s) | 0.98 s | 11.97-11.98 s |
| on, after this | 0.20-0.22 s | 0.03 s (0.017 s) | 0.99-1.02 s | 10.83-10.95 s |

The move is over within the quiet, and the job waits for nothing before
its recompute. Against 9.7 s for the recompute made here, what is left is
the worker's view meshes (0.6 s, sec 53), the put-in (0.65 s) and `Start`
(0.2 s). In exchange, the main thread is blocked 1.0 s instead of 9.8 s.

No test of its own: nothing a test can see changes. The shape restored
and its `InvalidShape` are the same either way, and only the time is not.
TestWorkerPool 78, Document 363, ShapeStorage 67, FileBlobs 97,
TestPartApp 446 and TestTechDrawApp 32 are OK, and ctest passes all 1285
that run.

## 59. A split leaves its cheap joins to be made here (2026-11-23)

The user, on sec 44's way through ("elaborate on 4", then "yes"). An
edit above the bodies reaches the assemblies, and an assembly reads every
body: the part holding the longest chain holds the assembly, so it holds
all the assembly reads, and no cut saves anything. Sec 44's `hole_radius`
edit of scanner was never split for that.

**Measured first** (`fcad-probes/worker/joinsplit.py`, console): each
object of a recompute here timed as `trackTimes()` times it, then sec 44's
split run in Python, once as it is and once with the joins left out.

| scanner | reached, time | as now: costliest part | with joins: costliest part + joins here |
|---|---|---|---|
| the `hole_radius` edit | 345, 8.47 s | 8.44 s | 6.54 + 0.06 s |
| every object touched | 686, 10.77 s | 10.55 s | 6.55 + 0.11 s |

The 6.5 s left is one chain, `PolarPattern003`'s body, which no split
cuts.

**The joins.** In `splitParts()`, an object is a join when its time is
known and at most `Workers/JoinSeconds` (0.05 s; 0 turns this off), it is
not risky (sec 56: a join is executed in this process), and every object
of the recompute that reads it is a join too. So nothing a part makes
reads a join. The recompute less its joins is split as before, and that
split is taken when its costliest part plus the joins' time here beats
the split without joins by `Workers/SplitSeconds`. An object no part
makes is already executed in the put-in (sec 42); the supplier now counts
a join apart (`JoinedHere`) and not as `ExecutedHere`, which stays the
count of what a worker should have made and did not. `SplitJoins` is the
joins of the last split. A recompute that is not split leaves nothing
out: one worker makes all of it, the joins too.

Test `testACheapJoinIsMadeHereSoWhatItGathersSplits`: sec 42's two slow
branches, each with its tail, and `Join` reading both tails. Without
joins it is one part (`JoinSeconds` 0 in the same test: one worker made
both branches). With them, `Join` and both tails are made here, each
branch by a worker of its own, at once.

**Measured, GUI** (`editsplit_gui.py`, scanner, the `hole_radius` cell
edited by 0.2 % and back, `SplitSeconds` 1 s for the split rows, two
workers allowed, warm):

| | result | main thread blocked | worker |
|---|---|---|---|
| here | 9.60-9.65 s | 9.77-9.81 s | -- |
| one worker | 12.60-12.64 s | 1.53-1.56 s | 9.11-9.17 s |
| split, joins here | 8.95-9.04 s | 1.55-1.60 s | 7.01-7.14 s |

The parts were 9 and 46 objects, with 298 joins, of which 172 were
executed (the rest, link elements and groups, come to no execute()). It
was expected to save 2.3 s and saved 3.6 s against one worker. The
second worker is faster than its estimate, and only 46 objects are put in
from workers where one worker puts in 165. The joins cost the main thread
nothing seen: it is blocked as long as with one worker. The split result
is the first worker result to come sooner than the recompute made here.
Compared object by object with here, it differs as the one worker does:
`SectionView007` (19 visible edges here, 20 from a worker), the TechDraw
kind of sec 43, open, and the last of four places in `Part012`'s area or
`Helix`'s volume. `Dimension014` is listed too, and in a recompute here
against another, but it reads nan every way: nan is not equal to itself.

At the default `SplitSeconds` then (5 s) this edit was not split: 2.3 s
expected. **The default is 2 s since** (the user, 2026-11-23, on these
numbers), and this edit now splits as it is.

TestWorkerPool 79 and Document 363 are OK.

## 60. SectionView007; a re-saved file asks for no recompute it had (2026-11-24)

**SectionView007** (sec 59: 19 visible edges here, 20 from a worker) is
not the worker's. This process gives 20 too after a save and a reopen,
and a full recompute after the reopen gives 19 again. Bisected, making
`Binder008` again alone (a `SubShapeBinder` of `Body004`, upstream of
`Fillet004`) gives 19. Made and restored, the binder differs in one line's
direction, by one ulp a component:

| Binder008, one line | direction | norm^2 - 1 |
|---|---|---|
| made | (-0.70710678118654735, 0.70710678118654757) | -1.1e-16 |
| restored | (-0.70710678118654746, 0.70710678118654768) | 0 |

Any read of a BRep moves it, a plain text one too: OCCT's reader makes a
`gp_Dir` of what it reads, and `gp_Dir` normalizes. A worker opens a saved
version, so it makes `Pocket022` and `Fillet004` from the restored
binder. The fillet differs about 1e-15, and the section's projection makes
of that one more edge, a B-spline 0.0133 long. Ruled out, each by a run
with it changed: the store's options (`DedupShapePCurves`,
`StableShapeBytes`, `DedupCongruentShapes`, `ShareStoredSubShapes`),
location chains, the pcurves, the XML's number precision (an exact writer:
still 20), `validateShape`, and the edges the fillet names (the same
subs).

**Mended in our OCCT** (the user: "do 3 then 1"). The readers, text and
binary, keep a direction as written when its square norm is 1 within 4
epsilon. They keep a conic's or an elementary surface's frame as written
when the frame `gp_Ax2`/`gp_Ax3`/`gp_Ax22d` would make of it is that frame
to rounding. occt `tests/fork/exchange/README.md`, "A direction read back
as it was written", has the driver, the suites run with each read, and the
fillet volumes that move: 31 of the fillet suite's 921 lines at 17 digits,
1.7e-7 relative at most, no verdict, no vertex. After it, the binder made
and the binder restored are the same but for the Checked flag, and
SectionView007 has 19 visible edges from a worker as here (`sv7.py`,
console: both values of the cell, by a worker and here).

The XML's numbers: `Base::Writer` writes a double fixed with 16 decimals,
so a value below about 1e-16 is written 0 and one below 1 loses digits.
Not the cause here. The writer went fixed at 12 decimals in 2011
(a65d4fdda6, no reason given) and to 16 in 2018 (714604e58f, forum
t=32212); bd89326d06 (2019) only spelled 16 `digits10 + 1`. The 17 digits
taken up in 2016 and dropped in 2019 (a3af2902ef, 183cd16263, forum
t=34392) were an expression's number text, which a user reads: at 17 digits
it shows its binary noise, 0.1 as 0.10000000000000001. The writer now saves
the shortest text that reads back exactly, sec 61.

**A re-saved file asked for its recompute again.** Scanner opened,
recomputed in full and saved, reopened with 18 objects `MigrationPending`,
each touched, and so did every open after, and every version a worker
opens. Two causes.

*A shape the recompute keeps.* 13 are shapes of naming 15 and OCCT 7.7.2
(`1.15.70200.4`) whose recompute does not set them: plain `Part::Feature`s
(`ShapeCopy`, `ShapeCopy001`, `ShapeCopy002`, `Solid004`, `Solid009`,
`Compound007`), detached binders (`Binder019`, `Binder031`), and Python
features whose class is not here (`Ray`, `Beam` to `Beam003`, of the
Optics workbench). `PropertyPartShape` writes the version it read until
its shape is set, so the save wrote 15 again. Now `Part::Feature::
recompute()`, ending without error for an object marked `MigrationPending`,
calls `keepMigratedNames()`. Each shape property whose read version
differs from this build's in the naming and the kernel alone takes this
build's (the table and the map format must be the same), and so does
`_ElementMapVersion` for the geometry, as a reopen would read it. Nothing
here makes those names again, and they read as they were written. Not
taken: a retained generation (`_BaseShape<N>`, which keeps its day's map
by design), and a Python feature whose `Proxy` is None. Its class, where
it is installed, may make the names again, so the five Optics objects
still ask.

*Held ids.* 5 (`Mirrored`, `Pocket021`, `Fillet003`, `Pocket022`,
`Fillet004`) were found to have names that crossed a document as text
(TransactionLogJournal sec 27.76 item 4). None of their names spells a
string with the old marker (`;:X;`, as `Binder008` wrote it before its
marker named its document). Each spells a string found again by its text,
and such a string keeps the related ids its first asking held
(`StringHasher::getID` matches data and postfix alone). String `0x14294`
spells `#14270;:M;CUT;:H75b:7,F`, and holds `0x8e5b`, which spells
`0x1694`, which carries the marker. `legacyStrings()` tainted a string
through every related id. Now only through the ids it is made of: the
encoded postfix, and the prefix of an indexed or prefix-id string. Those
it spells too, so this is a guard only. A held id is history, which the
string's text does not read, and no recompute could change it. On the
original file `Mirrored`, `Pocket021` and `Fillet003` are no longer found
crossed: they ask for their recompute for the version all the same.

Not mended: a plain feature whose name does carry the marker asks at every
open (test `testAShapeThatCrossedAsTextAsksForARecompute`): only making
the shape again could change the name, and that is the user's to do.

| scanner, recomputed, saved, reopened | before | after |
|---|---|---|
| `MigrationPending` | 18 | 5 (the Optics objects) |

Tests:
- `ComplexGeoDataTest.legacyCrossingIsReadInTheTextOnly` (C++): a
  name spelling a string that holds a marked one has not crossed; one
  spelling the marked one has.
- `testAShapeTheMigrationKeepsAsksOnce` (Document): a plain feature's
  shape rewritten to naming 15 asks at the open. Recomputed and saved, it
  asks no more and keeps its names.

**Measured, GUI, on that OCCT** (`editsplit_gui.py`, as in sec 59), each
run's 587 objects against the first recompute made here:

| | objects that differ |
|---|---|
| here, here back, here again | 0 |
| one worker | 0 |
| one worker again | 1: `Part012`'s area in the fourth place |
| split, split again | 1 each: `Part012`'s area, `Helix`'s volume, the fourth place |

`SectionView007` is gone from every row. The fourth-place differences of
sec 59 stay, and are not this.

## 61. A number saved as the shortest text that reads back as itself (2026-11-24)

`Base::Writer` wrote every double fixed with `digits10 + 1` = 16 decimals:
0.1 as `0.1000000000000000`, 1e-20 as `0.0000000000000000`, 12345.678 as
`12345.6779999999998836`. Below 1 that is short of the 17 significant
digits a double can need, so a value came back another double: of 100000
random values in (-1, 1), 31426 did; in (-1e-3, 1e-3), 99915 (Python,
`%.16f` read back). The history is in sec 60: fixed at 12 decimals in 2011
with no reason given, 16 in 2018; the 17-digit noise upstream backed away
from in 2019 was an expression's number text, which is not this writer.

**The change.** `Base::setSaveNumberFormat()` (`Writer.h`) sets a stream to
the save format, and every writer uses it: `ZipWriter`, `StringWriter`,
`FileWriter`, `NullWriter`, the char stream, and the transaction log's
`CaptureWriter`, which must write the bytes a save does (TransactionLogJournal
23.3). It imbues the classic locale with a `num_put` whose double is
`%.15g`, `%.16g` or `%.17g`, the first that `strtod` reads back as the
value (from 1 digit for a subnormal). The stream stays fixed at 16
decimals, which the shortest form stands for; a writer that sets another
precision for its own values (the texture and finish floats at 9) gets that
as before. No caller changed. A reader needs nothing: `atof` reads an
exponent, as every FreeCAD's does.

| value | before | after |
|---|---|---|
| 0.1 | 0.1000000000000000 | 0.1 |
| 1e-20 | 0.0000000000000000 | 1e-20 |
| 12345.678 | 12345.6779999999998836 | 12345.678 |
| 2 | 2.0000000000000000 | 2 |
| 5e-324 | 0.0000000000000000 | 5e-324 |

`sh1.py` saves 14 such floats, a float list, a vector list and a placement,
and reopens them: every one is the same double, bit for bit. Scanner
re-saved: the 9836 decimal tokens of its `Document.xml` are 118849 bytes,
182058 as `%.16f` would have written them (integral doubles, now written
without a point, are not counted). A small value costs more digits than
before, since it now has them: random values in (-1e-3, 1e-3) average 21.7
characters, 18.5 before.

**A crash on the way.** The first version kept the locale in a
function-local static. Two Part tests (`OffsetFixLevelTest`,
`FilletFixLevelTest.recomputePutsTheLevelBack`) end with their document
open and the transaction log's thread still capturing values; the static
was destroyed at exit, a `CaptureWriter` made after it imbued a dead
locale, and the thread crashed writing a string (`PropertyString::Save`,
`PropertyMaterial::Save`). The locale is now never destroyed; the two
passed 8 runs in a row.

Tests: `WriterTest.doubleIsShortestRoundTrip` (the table above, and 10000
values from 1e-20 to 1e20 read back exactly) and `doubleAtAnotherPrecision`.
Document 364, ShapeStorage 67, FileBlobs 97, TestWorkerPool 79, TestPartApp
446, TestPartDesignApp 1073, TestSketcherApp 182, TestTechDrawApp 32,
TestAnimation 78 OK; ctest 1287 with one failure, `FormWidgets_Tests_run`
under `-j 4`, which passed the 4 times it was run alone.

**Not the fourth-place differences.** The GUI comparison of sec 60
(`editsplit_gui.py`, scanner, `hole_radius` edited) on this writer, the
OCCT the same: one worker 0 of 587 differ; split and one worker again
`Part012`'s area 24940.2409 -> 24940.2408; split again `Helix`'s volume
978.5405 -> 978.5404. The same rows and values as sec 60, so a value the
old writer read back as another double does not make them.

## 62. The fourth-place differences are the sketch solver's history (2026-11-25)

The GUI comparison's last differences (sec 59, 60, 61: `Part012`'s area,
`Helix`'s volume in the fourth place) are not a worker's. The probe
alternates `hole_radius` between 5.5 and 5.511 mm and compares each run
with the run here at the same value, but not from the same history: the
reference and the worker each came to the value from a different previous
solution. Probes in `fcad-probes/worker/`.

**Here differs from here.** `fourth.py`, at 5.5 mm, shapes compared at 1e-9
relative: the file's own full recompute (A0) against a recompute here after
a trip to 5.511 and back (A1), against a worker after the same trip (A2),
against here after a second trip (A3). Each of A1, A2, A3 differs from A0,
and from each other, in `Helix` and `Helix001` only, by about 3e-8:

| | Helix volume |
|---|---|
| A0, full recompute | 978.1430212651425 |
| A1, here after a trip | 978.1430561870327 |
| A2, worker after a trip | 978.1430335472254 |
| A3, here after a second trip | 978.1430264757187 |

`Part012` is not among them at 1e-9; its area's fourth-place step in the GUI
is about 1e-5 absolute, `Helix`'s own (`Part012` holds the body). Touching
the whole chain and recomputing three times changes nothing
(`helixrepeat.py`), and one shape measures the same twice: the result
depends on where the solve starts, not on chance.

**Sketch034.** The first object of the chain to differ after a trip
(`helixtrip.py`), and in it one point (`sk034.py`): the start of arc 8 and
the end of line 10 on it move 1.87e-6 mm, where everything else moves about
1e-12. The sketch is fully constrained, but c21 puts that point on line 6,
to which the arc is (all but) tangent: the point is a circle meeting a line
that touches it, a double root. DogLeg stops when every residual is below
1e-10 (`DL_tolf`); along a double root the residual grows with the square
of the displacement, so the point is fixed only to about sqrt(1e-10), and
where it stops is where it came from -- the previous solution.

**From the same start a worker gives what here gives.** `samestart.py`: two
copies of the file, each recomputed in full and taken to 5.511 here (the
two then the same, every BRep); then back to 5.5, one here and one by a
worker. No vertex of any shape moves (Sketch034's at most 1.45e-15);
volume or area differ in the last digit for 2 of 269 (`Helix`
978.1430561870327 against ...329). The BRep text differs for 55, in form
only: a worker's shape has come through a BRep file, which keeps one
location where here a sketch holds a chain of 12, and one copy of a pcurve
that here is there twice (`ss_*.brep`).

Nothing to mend in the worker. A comparison of a worker with here holds only
from the same history; the GUI probe's does not, and its fourth-place rows
are this. Making the solver history-free here would need the point fixed
past the double root's square-root precision, which a tighter tolerance
narrows (1e-16 residual: about 1e-8) but does not close; the sketch itself
would be exact with c21 a tangency.
