# Per-user settings (entry 174): A DESIGN, NOT BUILT

Asked by the reporter 2026-10-19 09:03, through the note-taker's queue:
"lets design the per user settings. My thoughts, each edit shall identify
the initating user, prefwidget shall check current editting user and add
prefix to setting path. Check if settings in menu item can do the same".
It is the "later" of the 08:48 policy (docs/OnViewValueEditing.md 8.5):
several users edit one served document, each in a view of their own, and a
browser user is to have preferences of their own.

## 1. What exists (read in the source)

- **Who acts.** `App::Actor` / `App::ActorScope` (`src/App/Actor.h`): the
  actor of what this thread does while the scope lives, null for the
  desktop user. The Gui sets it while it replays a client's event
  (`SceneServeSource.cpp` 1901, 1934) or runs its operation
  (`SceneControl.cpp` 1388, `SandboxRemote.cpp` 136), from the roster
  (`SceneActors::of`). Kinds: `Verified` (an authenticating front door's
  identity), `Invited`, `Enrolled` (a browser a grant counts), `Declared`
  (a name nobody checked, `guest` when none), and `Local`, `Fork`. Today it
  names the author of a transaction-log row; nothing else reads it.
- **An edit does not record who began it** (`Gui::Document::setEdit`; no
  actor in `src/Gui/Document.cpp`). The edit session is shared by default
  (`PerViewEdit` off, `Document.cpp` 845); the task panel is one per
  document, mirrored to every client.
- **A preference widget** (`Gui::PrefWidget`, `PrefWidgets.cpp`) binds one
  parameter group once, by path (`setParamGrpPath` ->
  `WindowParameter::setGroupName`, which cannot change it after), reads and
  writes `entryName()` in it, and observes it. Its context menu's
  sub-entries (decimals, minimum, ...) go to a second fixed group,
  `BaseApp/WidgetEntries`, keyed by `entryPrefix()` (`PrefWidgets.cpp`
  231-294, 373+).
- **A parameter accessor class** (`MiscParams`, `PartParams`, ... generated
  by cog from `src/Tools/params_utils.py`) caches every value from fixed
  group handles, updates the cache from an observer, and hands out
  `const T&` (`MiscParams::getEnableCoarseSnap()`). The six drag settings of
  entry 171 are such accessors, read by `Gizmo.cpp` on every drag step.
- `ParameterGrp` has no fall-back: a group that lacks an entry returns the
  caller's default.

## 2. The design

### 2.1 Who "the user" is

**Revised after the reporter's answers of 09:32** ("it should never mix
with local user ... in addition to remember the user on editing, get the
user id from the view also if not in edit. Modify cog param helper to add a
flag to indicate the setting is per user ... check if that's possible"): no
writing to the local user's values, ever (2.2; a READ falls through to the
current setting, corrected 10:01); the user also comes
from the view (below); and 2.5 is the main road, with its feasibility
checked in 2.5.1.

A second thread scope beside the actor: `App::PrefUser` (an RAII scope,
nesting, innermost wins; in App, not Gui, because the generated accessor
classes of App and Base read it too), read by everything below. Kept apart from
`ActorScope` on purpose: the actor is who WROTE (the log's author must stay
the client who typed), the preference user is whose SETTINGS apply, and in a
shared edit those two differ.

- With no scope set, the preference user is `ActorScope::current()`; with
  neither, the desktop user -- today's paths, unchanged.
- **An edit records the user who began it**: `setEdit` keeps
  `ActorScope::current()` in the edit's state (null for the desktop user),
  next to the session. The reporter's "each edit shall identify the
  initiating user".
- **While an edit is open, its events run as its user's settings**: where
  the serve source replays a client's input into a view that is in an edit,
  and where an operation targets the edit's panel (the `widgets.*` ops), it
  sets `PrefUser` to the edit's user, inside the client's own
  `ActorScope`. So the panel's widgets and the handles' drag settings are
  the initiating user's, whoever of a shared edit's users touches them
  (2.6 b), and the log still names who touched them.
- **Out of an edit, the VIEW says who** (09:32): every viewer carries the
  user it is for -- a `MirrorViewer` its connection's actor, a desktop view
  none (the local user). Whatever runs for one view -- its events replayed,
  its render, its navigation -- runs under `PrefUser` = that view's user,
  unless the view is in an edit, whose user then wins. One user with
  several views is the same user in each.
- Code that runs for no view and no client (a timer, a recompute, a worker
  thread) has no preference user: the local user's values. A worker
  computing for a browser user therefore reads the local user's settings;
  a setting that matters there has to be read on the GUI thread and passed
  in.

### 2.2 Where a user's values live

- A parameter set of its own, `users.cfg` beside `user.cfg`, so the desktop
  user's file is not grown by every guest:

      User parameter:BaseApp/Preferences/Gui/Gizmos         (the global)
      Users parameter:<kind>/<name>/BaseApp/Preferences/Gui/Gizmos

  -- the reporter's "prefix to setting path": the user's prefix put before
  the path, the path itself unchanged below it. `<name>` escaped to a group
  name.
- **Read**: the user's entry when it has one, else the CURRENT setting
  (the local user's value as it stands), else the default -- corrected
  10:01: "for user value, just grab the initial value from the current
  setting. what I mean not to mix the local user is when write the
  settings". Read through, not copied: until the user writes a setting,
  they see the current one, so nothing is written on a mere read and a
  later change by the host still reaches a user who never touched it.
  (A copy taken once at first use is the other reading of "grab the
  initial value"; the read-through is proposed as the simpler, 2.6 g.)
- **Write**: the user's entry only, NEVER the local user's settings -- the
  "not mix" of 09:32. The local user never reads a browser user's value. A
  user "reset to default" removes their entry, so the current setting
  shows through again.
- When a user has no value of their own: a setting they never changed,
  their first visit, every connection of a `Declared` user (values not
  kept, 2.6 d), a setting a newer build added, a user the host forgot --
  the current setting in each case.
- **Who keeps values**: `Verified`, `Invited`, `Enrolled` users in the
  file. A `Declared` name is what a connection said and anybody can say it,
  so a declared user's values are held in memory for the connection only
  and dropped when it goes (2.6 d).

One helper does the lookup for both callers below:
`App::UserParameter::group(path)` -> under a preference user their group
(made on first use, in memory for a declared user), else the global one --
one group, never a blend of two.

### 2.3 Preference widgets

`PrefWidget` resolves its group through the helper instead of holding one
handle:

- restore: the user's read (the default when they have none); save: the
  user's write; observe the group it read.
- **Which user**: the preference user at the moment of the read or write
  (2.1). A panel's widgets are made inside `setEdit`, read again on every
  restore, and written under the edit's user by 2.1, so a panel shows and
  keeps its initiating user's settings.
- The sub-entries (`WidgetEntries`) go the same way, with the same prefix.
- **Which widgets**: those of task panels and edit menus -- user aware by
  default, since they are reached during an edit. The preferences dialog
  stays global: the desktop user's (whether a browser can reach it at all
  is not checked here). A widget can opt out
  (`userAware` property, false) for a setting that is the process's, not a
  person's (a cache size, the renderer).

### 2.4 Settings in menu items: yes, the same way

The answer to "check if settings in menu item can do the same": a menu
item's widget is a `QWidgetAction`'s check box or box, and what it reads and
writes is up to the code that builds it:

- `PrefWidget::buildContextMenu`'s items write `getEntryParameter()` --
  through the helper they are per user as in 2.3.
- Entry 171's Settings sub-menu (OnViewValueEditing 8.2) reads and writes
  the six `MiscParams` accessors -- per user once those accessors are (2.5).
- The menu itself is built under the preference user of the event that
  opens it (a right click replayed for a client, 2.1), so it shows that
  user's values; and a change made in it is written under the same user.

### 2.5 Accessor classes (`MiscParams` and the rest)

The hard part, as the note-taker saw: a getter is a cached `const T&` read
on hot paths (every drag step), with no user in its signature.

- **Opt-in per parameter**: `ParamBool('EnableCoarseSnap', True, ...,
  user=True)` in the `.py` declaration. Only such parameters cost anything;
  the rest are generated exactly as now.
- For one that is: the getter checks the preference user (one thread-local
  read); none -> the global cache as now; a user -> a per-user cache (a map
  from the user to the values, filled on first use from the user's group
  with the current (global) value standing in for a missing entry, updated
  by observers of the user's group and of the global one;
  a map node never moves, so `const T&` stays valid); the setter writes the
  user's entry under a user.
- A class can declare every parameter per user at once (`PerUser = True`
  beside `ParamPath` in its `.py`), the step-by-step road to "ideally, all
  settings shall be user aware": one class at a time, then the flag on the
  rest.
- First the six drag settings of 171, then whatever an edit reaches as the
  browser work (172) needs them. The generator change is one template in
  `params_utils.py`; the regenerated files are the cog blocks of each
  class.
- Python's `App.ParamGet` and code that reads a group directly stay global
  -- out of scope until a caller needs otherwise.

### 2.5.1 Is it possible in the generator? Yes (read in `params_utils.py`)

What `define()` makes today (`params_utils.py` 186-436): one hidden
`<Class>P`, a `ParameterGrp` observer with `handle` and `subHandles` (one per
sub-path), a member per parameter holding its cached value, a `funcs` map
from entry name to an `update<Name>` that re-reads it, and `OnChange`
dispatching through that map. The public statics return `instance()-><Name>`
and write through `handle`/`subHandles`. Every piece has a per-user twin
that the template can emit beside it, for the flagged parameters only:

- `struct UserValues { handle; subHandles; <the flagged members>; }`, and
  in `<Class>P` a `std::map<std::string, std::unique_ptr<UserValues>>` keyed
  by the user's id. Made on first use for a user: its handles from
  `App::UserParameter` (the user's prefix before `ParamPath` and each
  sub-path), each member read with the global member's current value as
  its default, and observers attached to the user's group and to the global
  one, a global change re-reading the user members with no entry of their
  own -- the same `funcs` table, re-pointed at the user's members (an
  `update<Name>` that takes the values struct instead of `self`).
- `get<Name>()`: `if (auto u = App::PrefUser::currentId()) return
  instance()->user(u).<Name>; return instance()-><Name>;` -- one
  thread-local read on the local path; a map lookup under a user.
- `set<Name>()` and `remove<Name>()`: the user's handle under a user.
  `default<Name>()` is unchanged.
- `on<Name>Changed()` and `signalParamChanged` fire for a user's change too,
  under that user's `PrefUser`, so a handler reading the getter sees the
  value that changed. Most flagged parameters have neither.
- Threads: the map is touched only under a preference user, which only the
  GUI thread sets (2.1); a mutex around the lookup costs nothing on the
  local path and keeps a stray reader safe.
- The parameter registry (omni search, `App::ParamRegistry`) builds editors
  from proxies that read and write the global group; a flagged parameter's
  proxy goes through the getter and setter instead, so an editor opened by
  a browser user edits their value. To be checked when built.
- Cost: nothing for an unflagged class (the template emits what it does
  today); a flagged class regenerates its cog blocks. The 47 classes whose
  output is committed are regenerated by hand (`CLAUDE.md`, cog).

### 2.6 For the reporter

a. The desktop user: no prefix, today's paths and file, unchanged?
   Proposed: yes.
b. A shared edit (one set of handles for all, 172's 9.5 a) driven by
   someone who did not begin it: whose settings? Proposed: the initiating
   user's, by your "each edit shall identify the initiating user"; the log
   still names who changed what.
c. ANSWERED 09:32, corrected 10:01: a user never WRITES the local user's
   settings; a user with no value of their own reads the current setting
   (2.2).
d. A `Declared` user (a name nobody checked): settings for the connection
   only, not kept? Proposed: yes; kept for verified, invited and enrolled
   users.
e. Which settings: task panels' and edit menus' widgets by default (with an
   opt-out), the accessor classes by opt-in per parameter? Proposed: yes.
f. One file for all users (`users.cfg`), or one file per user? Proposed:
   one file; a user's group is removed with them when the host forgets the
   user.
g. (10:01) "Grab the initial value from the current setting": read through
   until the user writes (a host's later change still reaches them), or a
   copy taken at their first use (fixed from then on)? Proposed: read
   through.

## 3. Order of work, once decided

1. `App::PrefUser`, a viewer's user, and the edit's user (2.1), with the
   serve source setting it for a client's events and its view's render; a
   test: an edit begun by a served client records it, a replayed event runs
   under it, a view out of edit under its connection's user.
2. `App::UserParameter` (2.2) and the declared users' memory store; C++
   tests, the no-mix rule among them (a user's write never reaches the
   local value, the local read never sees a user's; a user with no entry
   reads the current value).
3. The generator's `user=True` / `PerUser` (2.5, 2.5.1) and the six drag
   settings; 171's sub-menu then per user with no code of its own.
4. `PrefWidget` through the helper (2.3); a test with two served users of
   one document changing one panel setting each.
5. In the browser (with 172): two Chrome users, each their own coarse step.
