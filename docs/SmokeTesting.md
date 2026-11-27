# Smoke testing: finding what unattended tests miss (2026-11-26, proposed)

Why the hands-on rounds keep finding defects the suites pass over, and a rig to
find them without the reporter. Drawn from the 251 entries of
`docs/HandsOnQueue.md` (branch LinkVibe) and the reports of 2026-11-26. The
mapping of entries to patterns is by reading; nothing here is measured yet.

## 1. The patterns

Each is a gap between how a test runs and how the program is used.

| # | A test | The user | Entries |
|---|---|---|---|
| 1 | builds objects through the API | goes through commands, task panels, double click, Esc, Space | 250 (a scripted box has no edit, so no on-top order in the feed), 244, 236, 249, 113 |
| 2 | runs on a clean, normalised profile | has a theme, transparent overlays, a maximized window with the custom title bar, MSAA 4x, Direct3D, addons | 69, 85, 103 (reproduced only on a copy of the reporter's configuration), 49, 39, 43, the OpticsWorkbench worker errors and the title bar tool bar lock of 11-26 |
| 3 | makes fresh, small documents | opens old, large files: migration, properties a legacy file never had, references gone stale | 8, 14-18, 92, 105, 151, 247, PMI on a legacy Body (AddSolidBody off) and Dimension012 of 11-26 |
| 4 | checks the end state | sees what happens DURING a drag, a recompute, a load, a hover delay, a first use | 247 (a black view), 154, 84, 141, 26, 38, 158, 60, 238, 79 |
| 5 | exercises one feature | has several at once: overlay x TechDraw, overlay x mouse, selection x pre-selection x on-top, theme x everything | 86, 111, 119, 150, 124, 243, 245, 220 |
| 6 | asserts values | looks at pixels: jagged, low contrast, clipped, mirrored, a hit area that is not the drawing | 25, 64, 65, 77, 147, 246, the holes in a dimension's arrow heads of 11-26 |
| 7 | passes when nothing throws | notices what is silently wrong: an error only in the log, a wrong result, a command that did nothing | 105, 187, 205, 247 (no error box), a worker recomputing without an addon's code |
| 8 | is killed at its end | exits, reverts, undoes, reopens | 40, 131, 97, 166, 67, 251, the Draft BSpline reopen crash of 11-26 |

A ninth, of the tests themselves: a probe that measures the wrong thing.
Entry 48 compared a view with itself; the paint counts of 247 came from a
probe that skipped the drawing path; a body built from Python gave a wrong
measurement on 249.

## 2. The design

Not more per-feature tests: a rig that works as the user does, under checks
that must hold whatever it is doing.

### 2.1 How it runs (patterns 1, 2, 3, 5)

- **The staged copy**, not the build tree: what the reporter runs.
- **A clone of the reporter's profile**: `user.cfg`, `system.cfg`, the addons
  (the junction of `dl\test-addons\Mod`), `FREECAD_USER_HOME` set so the real
  profile is never written. On the hidden desktop, at the reporter's window
  size, maximized, the platform's renderer, MSAA as stored (the GUI test
  profile's `AntiAliasing 0` is NOT applied).
- **A corpus of legacy files**: a copy of `scanner.FCStd`, and documents of
  each older schema and of upstream's format. Never a document a script made
  in the same run.
- **Real events**: mouse and keys sent to the widgets, commands run by their
  buttons and shortcuts, task panels answered by their buttons. No
  `doc.addObject`, no `setEdit` from Python.
- **Profile switches varied pairwise**, not as a full product: theme
  (Classic, Dark, Light), overlay mode (off, opaque, transparent), render type
  (Default, Legacy), MSAA (off, 4x), PerViewEdit, navigation style, custom
  title bar, window maximized.

### 2.2 The checks (patterns 4, 6, 7, 8)

Each runs all the time, whatever the scenario; a scenario needs no claims of
its own to find something.

1. **The log.** No new Error or Warning in the Report view or the log file,
   against a baseline kept per document (scanner's Section003 is expected).
2. **Dialogs.** Every message box is recorded with its text and dismissed by
   the sweeper; one the scenario did not expect is a finding.
3. **Stalls.** A timer on the main loop; any gap over 300 ms is logged with
   the command or signal running. A budget per operation kind (a first use
   may take longer, and says so).
4. **Frames during work.** While a recompute, a load or a drag runs, frames
   are captured (`saveRenderDump`); none may be the cleared buffer, and the
   scene must not disappear between two that both show it.
5. **The hit area is the picture.** For each item on screen (a 3D element, a
   TechDraw item, a handle, a tool button), points sampled inside what is
   drawn must pre-select and select that item; points outside must not.
6. **Twins agree.** The same scenario two ways, compared:
   - the engine and Legacy (pictures, within a tolerance);
   - a worker and the main process (shapes, properties, the visibility
     census of `docs/WorkerProcess.md` sec 73);
   - saved and reopened against the live document (every property, the
     pictures);
   - Cancel, and undo to the start, against the state before (every property
     of both the document and the view providers).
7. **Layout.** No tool tip, handle, label or panel under an overlay panel or
   outside its window; no text clipped by its widget; text and icon contrast
   against their ground above a floor, in every theme.
8. **The end.** Exit code 0, no crash log, no dump, no directory left in the
   transient folder, no document open, the profile clone unchanged except
   for the keys the scenario set.

### 2.3 Where scenarios come from

- **The queue.** Each entry becomes a replay by the reporter's own steps,
  kept short. The checks of 2.2 run on it; its own claim is one more.
- **A walker.** Random, seeded and replayable: hover, click, double click,
  drag, wheel, Esc, Enter, undo, redo, a context menu's first item, over
  whatever is on screen, for a set time. It finds the combinations nobody
  wrote down. A finding is its seed and the last N steps.
- **Recorded rounds.** The command history and the transaction log of a
  hands-on round, replayed on the next stage.

### 2.4 Rules for a test, whatever the rig

- A fix's test FAILS on the stage it was reported on, by the reporter's
  steps, before it is called a test (the A/B the sessions already run).
- A probe is checked against a case known good and a case known bad before
  its numbers are believed.
- A GUI test that makes its objects by script says so, and has a sibling
  that goes through the command when the defect was reported from the GUI.

## 3. Order of work

Cheapest first; each step stands alone.

1. The existing `tests/gui` suite run a SECOND time under the profile clone,
   with checks 1 (log), 2 (dialogs) and 8 (the end) switched on. No new
   scenario; it shows how much pattern 2 and 7 alone are worth.
2. The legacy corpus loop: open, answer the migration, recompute, save,
   reopen, compare (checks 1, 2, 4, 6, 8).
3. The walker, with checks 3 (stalls) and 5 (hit area).
4. The pairwise matrix of 2.1 over steps 1 to 3.
5. Check 7 (layout, contrast) and the engine/Legacy twin.

## 4. Open questions

- Where it runs: nightly on the Windows box on the hidden desktop is the
  only place with the reporter's renderer; the Linux boxes can run the
  twins that do not need Direct3D.
- The baseline of check 1 for scanner: who keeps it, and how a new expected
  warning gets in.
- The tolerance of the picture twins; the engine and Legacy differ on
  purpose in places (face shading).
