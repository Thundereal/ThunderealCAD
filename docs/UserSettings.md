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

A second thread scope beside the actor: `Gui::PrefUser` (an RAII scope,
nesting, innermost wins), read by everything below. Kept apart from
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

### 2.2 Where a user's values live

- A parameter set of its own, `users.cfg` beside `user.cfg`, so the desktop
  user's file is not grown by every guest:

      User parameter:BaseApp/Preferences/Gui/Gizmos         (the global)
      Users parameter:<kind>/<name>/BaseApp/Preferences/Gui/Gizmos

  -- the reporter's "prefix to setting path": the user's prefix put before
  the path, the path itself unchanged below it. `<name>` escaped to a group
  name.
- **Read**: the user's entry when it has one, else the global entry, else
  the default. **Write**: the user's entry only; the global is the desktop
  user's (2.6 c). A user "reset to default" removes their entry, so the
  global shows through again.
- **Who keeps values**: `Verified`, `Invited`, `Enrolled` users in the
  file. A `Declared` name is what a connection said and anybody can say it,
  so a declared user's values are held in memory for the connection only
  and dropped when it goes (2.6 d).

One helper does the lookup for both callers below:
`Gui::UserParameter::group(path)` -> the global group and, under a
preference user, theirs; `get*(path, name, def)` with the fall-back;
`set*(...)` to the right one; and an observer of both.

### 2.3 Preference widgets

`PrefWidget` resolves its group through the helper instead of holding one
handle:

- restore: the fall-back read; save: the user's write; observe both groups,
  so a value changed under the global (the desktop user, the preferences
  dialog) shows in a user's widget that has no value of its own.
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
  from the user to the values, filled on first use with the fall-back read,
  updated by an observer of the user's group; a map node never moves, so
  `const T&` stays valid); the setter writes the user's entry under a user.
- First the six drag settings of 171, then whatever an edit reaches as the
  browser work (172) needs them. The generator change is one template in
  `params_utils.py`; the regenerated files are the cog blocks of each
  class.
- Python's `App.ParamGet` and code that reads a group directly stay global
  -- out of scope until a caller needs otherwise.

### 2.6 For the reporter

a. The desktop user: no prefix, today's paths and file, unchanged?
   Proposed: yes.
b. A shared edit (one set of handles for all, 172's 9.5 a) driven by
   someone who did not begin it: whose settings? Proposed: the initiating
   user's, by your "each edit shall identify the initiating user"; the log
   still names who changed what.
c. May a browser user change the GLOBAL value? Proposed: no -- a browser
   user writes only their own; the global is the desktop user's (the host).
d. A `Declared` user (a name nobody checked): settings for the connection
   only, not kept? Proposed: yes; kept for verified, invited and enrolled
   users.
e. Which settings: task panels' and edit menus' widgets by default (with an
   opt-out), the accessor classes by opt-in per parameter? Proposed: yes.
f. One file for all users (`users.cfg`), or one file per user? Proposed:
   one file; a user's group is removed with them when the host forgets the
   user.

## 3. Order of work, once decided

1. `Gui::PrefUser` and the edit's user (2.1), with the serve source setting
   it; a test: an edit begun by a served client records it, a replayed
   event runs under it.
2. `Gui::UserParameter` (2.2) with its fall-back and the declared users'
   memory store; C++ tests.
3. `PrefWidget` through it (2.3); a test with two served users of one
   document changing one panel setting each.
4. The generator's `user=True` (2.5) and the six drag settings; 171's
   sub-menu then per user with no code of its own.
5. In the browser (with 172): two Chrome users, each their own coarse step.
