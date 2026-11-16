# Worker processes -- a general facility, the Animation its first use

Status: design accepted 2026-11-16, every decision as recommended (sec
9). Built on branch `Worker`: phase 0 (`docs/Animation.md` sec 4.2),
phase 1 (sec 10), phase 2 (sec 11, 12), phase 4 (sec 13), phase 5a (sec
14); the order of what is left, as ruled 2026-11-16: phase 4, phase 5,
then a reduced phase 3.

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
| 3 | arena, blobs adopted by hash, string blocks + merge | per-frame files, import by content |
| 4 | move along the log on edits; jobs dropped by `reads` | restart at every edit |
| 5 | App-side mesh chunks; frames drawn from them; lazy materialise; the bake takes them | the read-back floor |
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
(sec 2); jobs dropped by `reads` (phase 4).

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
read-back with no change of behaviour; 5b (next) draws a frame without
materialising it.

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
  PartGui: for an object shown by a `ViewProviderPartExt`, "mesh
  <deviation> <angle>", as `updateVisual` takes them (the
  `OverrideTessellation` preference and the minimums applied). The
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
the view waited for, two runs each way: the replay (Time set) 76.5 ms
without the mesh, 33.4 ms with it; the whole frame 135 ms, 93 ms. A frame
from memory is unchanged (56 ms, of it 18 ms replay). The frame's file is
larger, 2.6 MB against 0.36 MB.

What is left of a frame read from disk, for 5b: the replay's 33 ms (the
element references, the restore, the view's fill of its nodes) and the
drawing after it.

Tests: TestAnimation 73 (a frame written meshed, near three times the
size, a hint it does not know written plain), `tests/gui/animation-
workers.py` 24 (a worker's frame holds the view's mesh; the pictures of
its frames those of the frames made here, as before).
