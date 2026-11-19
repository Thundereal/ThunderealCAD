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
  sketch's geometry ids, `Document::nextGeoId`, counted by the hook). A
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
execute may give geometry new ids, which makes a worker's job unfit (sec
19).

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
