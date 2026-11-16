# Worker processes -- a general facility, the Animation its first use

Status: design, 2026-11-16, for review. Nothing here is built except
phase 0 (branch `Worker`, `docs/Animation.md` sec 4.2), which this
replaces piece by piece.

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

Decisions wanted:

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
