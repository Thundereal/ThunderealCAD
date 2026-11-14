# Animation -- key frames for any property

Status: design, nothing built (2026-11-12; object model, section 3, elaborated the same day). Branch `Animation`, from `main`
3dd48e3f6d.

The goal: animate any property of any object -- a placement, a pad's
length, a joint's angle, a colour, a transparency, the camera -- with key
frames and a choice of interpolation per key, play it in the 3D view, scrub
it, record it by posing the model, and export it as video or to the web
viewer. One mechanism, not one per workbench.

## 0. Summary of the design

1. **An `App::Animation` object holds tracks; a track holds keys.** A track
   is aimed at one property, or one part of one (`Box.Placement`,
   `Box.Placement.Base.z`, `Pad.Length`, `Box.ViewObject.Transparency`),
   addressed by an `ObjectIdentifier` -- the path the expression engine
   already resolves and keeps across renames. A key is a time, a value and
   the interpolation of the span that leaves it. All tracks and keys live
   in one property of the Animation, not one document object per key.
2. **Time is in seconds**, a `double`. The frame rate matters only for
   playback, snapping and export.
3. **Interpolation per key**, for the span to the next key: Constant (step),
   Linear, Bezier (with handles), Easing (the Penner set the old branch
   took from `QEasingCurve`, with its In/Out/InOut and amplitude, period,
   overshoot), and Python (a callable, as the old `UserEasingCurve`).
   Extrapolation per track before the first key and after the last:
   Constant, Linear, Cycle, Cycle with offset, Ping-pong.
4. **Each property type says how it is animated**, through a small codec on
   `App::Property`: the numbers it is made of, and what kind each group is
   (scalar, angle, rotation, colour). Curves run on numbers; a rotation runs
   as a quaternion. A type with no codec (string, link, shape) is stepped.
   The existing `Property::interpolate()` stays and is written on top.
5. **Playing does not edit the design.** Values written while an animation
   plays are written in a scope (`App::WriteScope` of `Animation`, sec
   5.1) that opens no transaction, is not logged, and
   does not mark the document modified. The values the animation displaced
   are kept and put back on stop, and a save writes them, not the pose of
   the moment.
6. **`Animation.Time` is a property expressions can read.** That gives
   procedural animation (Blender's drivers) at no cost:
   `Gear2.Placement.Rotation.Angle = -2 * Anim.Time * 30 deg`. The
   Animation keeps its targets by path, without a dependency edge, so
   reading `Time` never closes a cycle.
7. **Recording is auto-keying.** With Record on, each change the user makes
   to a property keys it at the current time. That keeps the old branch's
   way of working -- pose the model, capture -- over a model where every
   property keeps its own key times.

Items 1, 2 and the name were confirmed by the user on 2026-11-12, with
time in seconds (section 12). The rest of the document is why, and how.

## 1. What the old branch did

`origin/Animation` (realthunder/FreeCAD, 2024-10-28, `12b54fb812` "[WIP]:
App::Animation" over `5da8da4ec8` "App: add API Property::interpolate()").
The interpolate commit is already in this tree (`src/App/Property.h:407`,
overridden for Integer, Float, Vector, Direction, Placement, Rotation,
Color and their lists); nothing calls it yet. The rest was not merged.

The model, in short:

- **`App::KeyFrame`** is a document object, one per frame. While its
  `Recording` is on, a Gui `AnimationManager` listens to
  `signalBeforeChangeObject`/`signalChangedObject` and copies every changed
  property that `canInterpolate()` into a hidden dynamic property of the
  frame, named by an encoding of its path (`_3Box_9Placement`); the value
  before the first change goes to the Animation as the "Original".
- **`App::Animation` is itself a `KeyFrame`** with an ordered `Frames` list,
  so animations nest. Its `Duration` is the sum of its frames'.
- A frame has `Duration`, `Delay`, a `Trigger` (After previous, With
  previous, On click -- a presentation's terms), `Jump` and `NextFrame`
  (branching), `Progress` (0 to 1), and its easing: `EasingCurve` (the 41
  `QEasingCurve` types), `EasingPeriod`, `EasingAmplitude`,
  `EasingOvershoot`, or a Python `UserEasingCurve`.
- Playing a frame interpolates each recorded property from the previous
  frame's value to this frame's by the eased progress, then recomputes
  unless `NoRecompute`. `FrameRate` and `AutoSkipFrame` choose between every
  frame and real time; `ExcludeProperties`, `CacheProperties` and
  `EnableCache` were declared and not used.
- Commands `Std_AnimationNew`, `NewFrame`, `Play`, `Pause`, `Stop`, `Prev`,
  `Next` in a `Std_AnimationGroup`, with nine icons; `gotoPrev`/`gotoNext`
  were empty.

What to keep: the interpolate API (extended, section 4); the easing list and
its three parameters; the Python easing callable; capture by posing; the
Trigger/Delay/Jump sequencing (it becomes strips, section 7); real time
against every frame; leaving recompute out; the command names and icons.

What to change, and why:

| Old | Problem | New |
|---|---|---|
| A document object per key frame | A 10 s, 30-key animation is 30 objects in the tree, 30 objects in every transaction and log snapshot | One property holds every track and key |
| A frame is a snapshot of every recorded property | Every property shares the same key times; a lid that opens while a light dims needs a frame at each change of either | A track per property, each with its own key times |
| Interpolation between neighbouring frames only | No smooth curve through three or more keys; no handles | Curves over all of a track's keys |
| Paths encoded into dynamic property names | Breaks on rename; no sub-path (`Placement.Base.z`); no view properties | `ObjectIdentifier`, as expressions use |
| `Progress` 0..1 per frame | Time is relative, frames of different length shift each other | Absolute time in seconds |
| Values written as ordinary edits, in a transaction ("Play animation") | Playing fills the undo stack and the transaction log, and marks the file modified | `App::AnimatedWrites` (decision 5) |
| `QEasingCurve` in App | Ties evaluation to Qt; glTF export bakes curves anyway | Own easing functions, same names and parameters |

## 2. How others do it

**Blender** is the reference for keyframes on arbitrary properties.
- An *Action* holds *F-Curves*. Each F-Curve is aimed at an RNA data path
  plus an array index (`location`, 2), so a vector is three curves.
  Since 4.4 an Action has *slots*, one per animated data-block, so one
  Action can animate several objects; the F-Curves of each slot sit in a
  "channelbag", and an Action has one layer and one strip for now
  ([layered actions](https://developer.blender.org/docs/features/animation/animation_system/layered/)).
- A keyframe is a Bezier point with two handles. Handle types are Auto
  Clamped (the default), Auto, Vector, Aligned and Free; Vector gives
  straight segments ([keyframes](https://docs.blender.org/manual/en/4.2/animation/keyframes/introduction.html)).
- The interpolation mode is set per keyframe and governs the span to the
  next key: Constant, Linear, Bezier, or one of the easing types (Sine,
  Quad, Cubic, Quart, Quint, Expo, Circ, Back, Bounce, Elastic) with Ease
  In/Out/In-Out ([F-Curve editing](https://docs.blender.org/manual/ar/4.4/editors/graph_editor/fcurves/editing.html)).
- Extrapolation is per curve (Constant, Linear), and *modifiers* add cycles,
  noise, stepping and limits.
- *Drivers* set a property from an expression of other properties or the
  frame. The *NLA* lays actions out as strips that can be offset, scaled,
  repeated and blended.
- Keys are put in with `I` over a property, by auto-keying (a record
  button), or by keying sets. A keyed property is drawn yellow, or green
  between keys, and orange when it was changed and not keyed.
- Rotations: Euler channels can turn many times but suffer gimbal effects;
  quaternion channels are interpolated per component and normalised.
- Time is in frames, with subframes.

**SOLIDWORKS motion study** (Animation and Basic Motion). A timeline with
key points per component, its appearance, and the view orientation. The
interpolation mode is set on the key point
([interpolation modes](https://help.solidworks.com/2021/english/SolidWorks/motionstudies/t_changing_interp_modes_anims.htm)):
- Linear, the default, is uniform speed.
- Snap holds the value until the next key.
- Ease In speeds up towards the next key, and Ease Out slows down towards
  it.
- Ease In/Out speeds up to halfway and then slows down.

Mate dimensions can be keyed, so a parametric distance animates and the
model rebuilds. Autokey captures drags. Motors and physics are a separate
study type.

**Inventor Studio.** It animates the following on one timeline:
- components
- constraints, by driving their value
- user parameters, which change the geometry through recompute
- fades
- cameras
- lights

Each action has an acceleration profile, either constant speed or a
velocity with ramps
([produce animations](https://help.autodesk.com/cloudhelp/2026/ENU/Inventor-Help/files/GUID-BE01AA75-8B7B-4E2D-833C-0385F7A1B165.htm)).
A constraint that holds a component still blocks animating that component
until it is suppressed.

**Fusion animation workspace.** Storyboards, several per design, each with
a timeline of actions:
- transform components
- auto explode
- show and hide
- the view (camera)
- annotations

Reversing a storyboard gives the explode/assemble pair
([deep-dive](https://www.autodesk.com/products/fusion-360/blog/fusion-360-animation-deep-dive/)).
The animation leaves the design itself untouched: the positions live in
the storyboard.

**glTF and USD**, as the interchange the web tier speaks.
- A glTF animation is a set of channels, each aimed at a node's
  translation, rotation, scale or morph weights. Each channel has a sampler
  with keys and one of LINEAR, STEP or CUBICSPLINE; rotations are
  quaternions interpolated by slerp.
- USD gives any attribute time samples.

Everything else, easing included, is baked to samples on export.

What we take from them:

1. Blender's split: an address (path), keys on it, and the interpolation
   of a key's outgoing span. That covers every CAD tool's set: Snap is
   Constant, and the three Ease modes are Bezier with flat handles or
   Easing Sine/Cubic.
2. The CAD tools' rule that the animation is not the design (Fusion
   outright, the others by keeping the study apart). This decides
   section 5.
3. Parameters and constraint values are keyable and rebuild the model
   (SOLIDWORKS, Inventor). We get them for free: they are properties.
4. Drivers already exist here as expressions. Only `Time` is missing.
5. A camera track and a visibility/fade track are expected by every CAD
   user.
6. Storyboards (Fusion) and the NLA (Blender) are the same thing at
   different depths: several animations, placed in time. That is phase 4.

## 3. The object model

Decided by the user, 2026-11-12: the CAD rule for rest values (section 5),
one property for all tracks, time in seconds, the name `App::Animation`.

### 3.1 The pieces at a glance

```
App::Document
 +- App::Animation "Anim"                 the only document object of the feature
 |    Tracks   App::PropertyAnimationTracks    every track and key, one value
 |      track 1  Property  Box.Placement             keys at 0, 2, 5 s
 |      track 2  Property  Pad.Length                keys at 1, 3 s
 |      track 3  Property  Box.ViewObject.Transparency
 |      track 4  Camera    view 1                    (a Gui handler, 3.6)
 |    Time Start End FrameRate Speed Loop Recompute Exclude Active   saved
 |    Recording                                                        transient
 |    (runtime, never saved: the rest store, the resolved targets)    (3.7)
 +- Gui::ViewProviderAnimation            tree item; double click activates

Gui, one per process
 +- Gui::AnimationTimeline                a dock window, so it can be an overlay (3.10)
 +- Gui::AnimationPlayer                  the wall clock: steps the active document's
 |                                        active Animation while playing
 +- Gui::CameraTrackHandler               handles tracks of kind "Camera"
```

Only the Animation is a document object. Tracks and keys are values
inside one of its properties:
- They have no names in the document, no tree items and no view
  providers.
- They have no transactions of their own: an edit of a key is an edit of
  `Anim.Tracks`.
- They do have ids that stay the same across edits, undo and save, so the
  timeline's selection and a Python reference survive an undo.

### 3.2 `App::Animation`

| Property | Type | Status | Meaning |
|---|---|---|---|
| `Tracks` | `PropertyAnimationTracks` | | 3.3 |
| `Time` | `PropertyTime` (s) | NoModify, NoRecompute | The time shown. Setting it to another time evaluates (section 4); a write of the time already shown is no change to any property (`Property::hasSetValue` drops it), so `activate()` is the way to evaluate at it. NoModify keeps a scrub out of the undo stack and the transaction log and does not mark the document modified, as for `Visibility` (TransactionLogDesign sec 2.4). Saved, so a file opens where it was left |
| `Start`, `End` | `PropertyTime` | | The play range. `End` 0 means the last key's time |
| `FrameRate` | Float, frames per second | | 30. Playing and snapping use it; keys are not tied to it |
| `Speed` | Float | | 1. Playback only |
| `Loop` | Enumeration | | Once, Loop, PingPong |
| `Recompute` | Enumeration | | Touched (default), Never (view only: the old `NoRecompute`), Full |
| `Exclude` | StringList | | Paths that Record never keys (the old `ExcludeProperties`); `*` patterns allowed |
| `Active` | Bool | NoModify | At most one per document. Setting it on clears it on the others, and they put their rest values back first. Only the active one evaluates |
| `Recording` | Bool | Transient, NoPersist | Record (section 6). Off on open |

There is no `Playing` property, unlike the old branch. Playing is the
clock's business, in the Gui (3.9); the document knows only the time.

What App provides headless:
- `evaluate(t)` writes every track's value at `t`;
- `value(track, t)` evaluates without writing;
- `activate()` and `deactivate()`, which restores the rest values;
- `frames()` yields each frame time of the range at `FrameRate`, the export
  loop, with no clock involved.

Every key edit is a method of `Tracks`, as follows.

`execute()` does nothing. The Animation is never touched by its tracks
and is never part of its targets' dependency graph.

### 3.3 `App::PropertyAnimationTracks`

A `PropertyXLinkContainer`, the base under the expression engine. It
gives rename and relabel handling, cross-document targets and deleted-object
notices without writing them again. Every target object is registered
as a dependency with `LinkScope::Hidden` -- `hiddenref()`'s scope, which
makes no edge in the dependency graph. So:

- An expression that reads `Anim.Time` from an object the Animation drives
  is not a cycle.
- Recomputing a target never recomputes the Animation, nor the reverse.
- The document knows the Animation refers to its targets: deleting a
  target notifies the property (`onBreakLink`), and copying the Animation
  into another document re-points the paths as expressions are
  re-pointed.

The value is `std::vector<Track>` in the timeline's order (the user can
reorder), plus the next free track id.

Edits are methods, each one atomic: `aboutToSetValue()`, change, then
`hasSetValue()`:

```
int  addTrack(const ObjectIdentifier &target, kind = "Property");   // -> id
void removeTrack(int id);
int  setKey(int track, double t, const AnimValue *v = nullptr,      // -> key id
            const KeyOptions *opts = nullptr);   // null value: the target's current one
void removeKey(int track, int key);
void moveKeys(const std::vector<KeyRef> &, double dt);   // a drag in the timeline
void setInterp(const std::vector<KeyRef> &, Interp, const Easing * = nullptr);
void setHandles(KeyRef, Handle left, Handle right);
void setTrackOptions(int track, const TrackOptions &);
```

Each call from a command or a finished drag is one undo step. That
follows the fork's rule that an edit's steps are one undo
(MultiViewEdit.md 12.11).

`copyBeforeChange()` copies the vector, and `isSame()` compares it. A
thousand keys is tens of kilobytes. The transaction log stores the
property's value whole; rows per track can come later if large animations
need them.

Saved inside the Animation's object element:

```xml
<Property name="Tracks" type="App::PropertyAnimationTracks">
  <Tracks nextId="5">
    <Track id="1" kind="Property" target="Box.Placement" valueType="App::PropertyPlacement"
           before="Constant" after="Constant" rotation="Shortest">
      <Key id="1" t="0" interp="Bezier" v="0 0 0 0 0 0 1"
           left="AutoClamped" right="AutoClamped"/>
      <Key id="2" t="2" interp="Easing" easing="Bounce" mode="Out"
           amplitude="1" period="0.3" overshoot="1.70158" v="0 0 50 0 0 0.7071 0.7071"/>
    </Track>
    <Track id="2" kind="Property" target="Pad.Length" valueType="Quantity" unit="mm">
      <Key id="1" t="1" interp="Linear" v="10"/>
      <Key id="2" t="3" interp="Linear" v="40"/>
    </Track>
    <Track id="3" kind="Property" target="Link.LinkedObject" valueType="App::PropertyXLink">
      <Key id="1" t="0" interp="Constant"><Value><![CDATA[...the property's own XML...]]></Value></Key>
    </Track>
  </Tracks>
</Property>
```

The numbers are written with the same round-trip precision as
`PropertyFloat`. A stepped key carries its property's own `Save` text, so
any type can be stepped, a Python one included.

Built, phase 2 (`App/Animation.{h,cpp}`), with these differences:
- Each `<Track>` also carries `layout` ("S3 Q4": the groups of numbers) and
  `count`. The layout cannot always be derived from `valueType`: a list's
  depends on its length, and a list whose length has changed since is
  then `Mismatch`.
- A stepped key's text is an attribute, `text="..."`, encoded, rather than
  CDATA, which the text itself may contain.
- Edits are the methods above with explicit values (`setKey(track, key)`);
  reading the target's current value is the Animation's (`Animation::
  setKey(track, t, value)`), which resolves the path.
- A track's path is re-pointed as an expression's is (2026-11-14): to the
  copy an import of external objects makes (`CopyOnImportExternal`), to
  the object a link replaces in a group (`CopyOnLinkReplace`, the same
  rule as `ObjectIdentifier::replaceObject`), and to a new label when a
  path names an object by label (`<<Alpha>>.Val`: the labels a path names
  are registered with the property as an expression's are, so
  `CopyOnLabelChange` reaches it). A path by name is not touched by a
  relabel. Tests: `Animation_tests_run`, `AnimationTrackPaths`.

### 3.4 Track

```
struct Track {
    int id;                    // stable, never reused in the document
    std::string kind;          // "Property" (App) or a registered handler (3.6)
    ObjectIdentifier target;   // Property kind: Box.Placement, Pad.Length,
                               // Box.Placement.Base.z, Box.ViewObject.Transparency
    std::string handlerData;   // other kinds: what the handler needs (the camera's view)
    std::string valueType;     // the target's type when first keyed, and a unit for Quantity
    std::string label;         // optional, shown in the timeline
    bool mute = false, lock = false;
    Extrap before = Constant, after = Constant;   // Constant | Linear | Cycle | CycleOffset | PingPong
    RotationMode rotation = Shortest;             // Shortest | Turns | Euler (3.12)
    std::vector<Key> keys;     // sorted by time
    int nextKeyId;
};
```

**One track per target; whole and parts may both be there** (user,
2026-11-12). A property can have a track on all of it (`Box.Placement`) and
tracks on parts of it (`Box.Placement.Base.z`,
`Box.Placement.Rotation.Angle`) at once. That lets a box follow keyed poses
while its height runs on a curve of its own. Two tracks on the same path
are not allowed; a second one is the first.

**Parts apply after the whole**, the shorter path first, then the longer
ones in turn. So `Placement`, then `Placement.Rotation`, then
`Placement.Rotation.Angle`; parts of the same length (`Base.x`, `Base.z`)
do not overlap, and their order does not matter.
- At time t, the whole track's value is evaluated, each part's value
  replaces its piece of it, and the result is written to the property
  once (section 4).
- Where a part has no key yet or is muted, the whole track's piece stands.
- A part that turns its piece's value outside the whole one's -- `Angle`
  over a whole `Rotation` -- keeps the whole's axis and replaces the
  angle. Each part replaces exactly what its path names, nothing more.

The timeline shows part tracks indented under the whole track of the same
property, or under the property's name when there is no whole track.

**A track's state** is computed when the track is resolved, and is shown
in the timeline and to Python:

| State | When | What evaluation does |
|---|---|---|
| Ok | The target resolves and matches `valueType` | writes |
| Unresolved | The object is deleted, the document is closed, or the dynamic property was removed | skips. The track is kept; an undo of the deletion resolves it again. "Remove unresolved tracks" cleans up |
| Mismatch | The property now has another type | skips; re-keying converts or the user removes it |
| Driven | The property is bound to an expression | writes, and warns (3.11) |
| Solved | The target is a placement an Assembly solves | writes, and warns (3.11) |

### 3.5 Key

```
struct Key {
    int id;                     // stable within its track
    double t;                   // seconds; keys closer than 1e-6 s are the same time
    AnimValue v;                // 3.12 for what the numbers are
    Interp interp = Linear;     // for the span to the next key:
                                // Constant | Linear | Bezier | Easing | Python
    Easing easing;              // type Sine..Bounce, mode In|Out|InOut|OutIn,
                                // amplitude 1, period 0.3, overshoot 1.70158 (the old defaults)
    Handle left, right;         // Bezier: AutoClamped|Auto|Vector|Aligned|Free,
                                // dt (s) and dv per scalar component; Auto* computed, not stored
    std::string python;         // Python: an expression in u, or a callable's dotted name
};

struct AnimValue {
    std::vector<double> nums;   // the target type's numbers (3.12)
    std::string text;           // a stepped key: the property's own saved text
};
```

The default interpolation for a new key is the Animation's preference
(`Linear`), and Record uses it. Bezier handles on direction and quaternion
groups are not stored: they are always computed (Auto), and the curve runs
on the components and is normalised, as Blender does (3.13). Free handles
on rotations would need a handle on the sphere, which no CAD user asked
for.

Moving a key onto the time of another key of the same track replaces it,
as in Blender. The timeline snaps to frames, but the model does not:
a key at 1.0333 s stays there when the frame rate changes.

### 3.6 Track kinds other than properties

Some things animated are not properties of a document object. The camera
is the one needed now, and an Assembly simulation run played as a track
is a later one. For these, App holds the keys and a registered handler
reads and writes the value:

```
class AnimationTrackHandler {   // App, registered by kind name
    virtual bool layout(const Track &, AnimLayout &) const = 0;
    virtual bool read(const Track &, AnimValue &) const = 0;      // the current value
    virtual bool write(const Track &, const AnimValue &) = 0;     // under AnimatedWrites
    virtual std::string describe(const Track &) const = 0;        // timeline label
};
```

The Gui registers "Camera" (section 8). In a headless process no handler
is registered, so camera tracks are Unresolved: skipped, kept and saved.

### 3.7 Runtime state, never saved

- **The rest store**: property to the value it had before this
  Animation's first write to it since activation (section 5). It is kept by
  property, not by track, because a whole track and its part tracks write
  the same property.
  - Filled lazily, so activating costs nothing.
  - Emptied by `deactivate()`, which writes the values back and then
    recomputes.
  - A track added while active stores its target's rest value when it
    first writes.
- **The resolved targets**: per track, the object, the property and the
  path components. Built, phase 2: resolved afresh at each evaluation --
  a few name lookups per track, microseconds -- so there is no cache to
  invalidate; one comes if large animations show the cost.
- **The last written value** of each property, beside its rest value.
  Whether the property still holds it tells whether anybody else wrote it
  since -- an edit, an undo, a recompute -- and so whether its value now is
  the design's and becomes the new rest value (5.2).
- **The last evaluated time**, so an unchanged `Time` does not evaluate
  twice.

### 3.8 What happens when

| Event | Effect |
|---|---|
| A key is added, moved or deleted | One transaction on `Tracks`; the Animation re-evaluates at `Time` if active |
| Undo or redo of a key edit | `Tracks` restored; the timeline keeps its selection by key id; re-evaluates |
| Any other undo or redo while active | Undoes the design without stopping: a transaction records an animated property's rest value as its value before an edit (5.2), so an undo puts the design back, and the next evaluation shows the pose again |
| A target object is deleted | Its tracks become Unresolved; its rest entry is dropped |
| The deletion is undone | They resolve again |
| A target's document closes | Unresolved; nothing to put back |
| Save while active | Stop with recompute, save, re-activate at `Time`. The recompute is what makes it right: a saved Pad must hold a shape of its saved Length, not of the animated one |
| The Animation is deleted | Deactivates first, so the design is as it was |
| The Animation's document closes | Nothing written back; the file was saved with rest values anyway |
| The file is opened | Tracks restored and resolved after the document's restore; an `Active` Animation evaluates once at `Time`, when the document is whole (`signalFinishRestoreDocument`) |
| The Animation is copied to another document | Paths re-pointed as expressions are; targets left in the first document become cross-document |
| Another Animation is activated | This one deactivates first |

### 3.9 Gui objects

- **`Gui::ViewProviderAnimation`**: the old icons; no children in the
  tree, because keys are not objects. Double click, or `setEdit` Default,
  makes the Animation active and raises the timeline. That is all its
  "edit" is: no task dialog is opened (3.10). The context menu has
  Activate, Play, Record, Stop and Remove unresolved tracks.
- **`Gui::AnimationTimeline`**: a dock window registered with the
  `DockWindowManager`, so the fork's overlay mode applies to it like any
  other dock. It shows the active Animation of the active document, or a
  "no animation, create one" line.
  - Phase 3 contents: transport buttons, the time ruler and scrub bar,
    the range, a row per track with its keys (a dope sheet), and a side
    panel for the selected keys' interpolation and easing.
  - Phase 4 adds the curve editor.
- **`Gui::AnimationPlayer`**: one per process, holding a timer at
  `FrameRate`.
  - Playing advances `Time` by the wall clock times `Speed`, and skips
    ahead when a frame overruns.
  - Pausing stops the clock and keeps the pose.
  - Stopping puts the rest values back, by the rule of 5.2, and turns
    `Active` off (built, phase 2: `stop()`), so a file saved stopped opens
    stopped. The timeline keeps the Animation as its chosen one; the next
    scrub or Play activates it again.
  - One player per process, and one Animation playing at a time.
- **Property editor**: a row whose path a track targets is marked with
  key at this time, between keys, or changed and not keyed. Its context
  menu has Insert key, Delete key and Clear track (section 6).

### 3.10 The timeline is not the Animation's task panel

A task panel was the first thought, and it would be the natural FreeCAD
idiom for "editing an object". It does not fit, for four reasons:

1. **One task dialog per process.** The fork shows at most one dialog at
   a time (TaskPanelPerView.md sec 8), because there is one active
   transaction. With the timeline holding the dialog, no other could open:
   - a sketch,
   - a Pad's panel,
   - the transform dragger's panel.

   Posing the model under Record needs exactly those.
2. **A dialog is one transaction until OK or Cancel.** The timeline stays
   open for an hour of work. Its key edits are each an undo step, and
   Cancel rolling back an hour of keys would be wrong.
3. **Playback and scrubbing are wanted beside everything else**, including
   while another object is in edit, as in every tool of section 2.
4. **Shape.** A timeline wants the window's width under the 3D view, the
   bottom dock area. The task panel lives in the combo view or at the
   side of its view.

On point 4 of the open questions: agreed, a dock window can be an overlay
like any other, so there is no choice to make. It is a dock window.

When per-owner transactions land (`origin/Transaction`) and several dialogs
may be open at once, the same timeline widget can also be offered as a
task page, if wanted. Nothing in the model depends on where it is shown.

### 3.11 Expressions, Assembly and Links

- **A conflict is warned, never skipped** (user, 2026-11-12). Two cases
  have something else writing the same property:
  - **Driven:** the property is bound to an expression.
  - **Solved:** it is the Placement of a part an Assembly solves.

  In both, keys are inserted, Record keys it, and evaluation writes the
  animated value as for any track. The conflict is shown in orange:
  - on the track's row in the timeline, with the reason as its tooltip;
  - on the property's row in the property editor;
  - for Driven, on the expression itself: its row, and the expression
    editor when it is opened.

  What the user then sees follows from the order of the writes:
  - **Driven.** With `Recompute` Never, the animated value shows. With
    Touched or Full, the recompute evaluates the expression afterwards,
    so the expression's value shows. To animate a driven property for
    good, write the expression in terms of `Anim.Time`, as SOLIDWORKS and
    Inventor drive a driven dimension.
  - **Solved.** The solver starts from the current placements. Along a
    part's free degrees of freedom the animated placement stays, as a
    drag would. Along constrained ones the solver puts it back. So keying
    a solved part's Placement moves it where its joints let it move,
    which is useful in itself. To move it along a constrained degree of
    freedom, key the joint's value instead.
- **Joint values** (a Distance joint's offset, a Revolute's angle) are
  ordinary Float/Quantity properties. They are keyed like any other, and
  `Recompute` Touched runs the solver per frame. A grounded part, or one
  in an assembly whose solving is off, is not Solved.
- **Links.** A Link's Placement is the Link's own property, so a track
  animates that instance and leaves the linked object and its other
  instances alone. A link array's element is keyed by its index sub-path
  (`Array.PlacementList[3]`).
- **Spreadsheets.** A cell is a sub-path (`Sheet.Length`, an alias), so a
  spreadsheet-driven model is animated by keying one cell.

### 3.12 Values: how a property type is animated

The animation codec is added to `App::Property` (built, phase 1;
`App/AnimationCurve.h` has the layout):

```
namespace App::Anim {
enum class Kind { Scalar, Direction, Quaternion, Colour };
struct Group { Kind kind; int size; };
struct Layout {
    RotationMode rotation;      // asked for by the caller: how a rotation is written
    std::vector<Group> groups;  // e.g. Placement: {Scalar 3}, {Quaternion 4}
};
}
virtual bool getAnimLayout(Anim::Layout &) const;     // false: stepped
virtual bool getAnimValue(const Anim::Layout &, std::vector<double> &) const;
virtual bool setAnimValue(const Anim::Layout &, const std::vector<double> &);
```

There is no Angle kind: an angle is a scalar in degrees, and nothing
blends it differently. Stepped is the absence of a codec, not a kind.

| Property | Layout | Notes |
|---|---|---|
| Float, Quantity (Length, Angle...) | Scalar 1 | FloatConstraint and QuantityConstraint clamp to their bounds on write |
| Integer, IntegerConstraint, Percent | Scalar 1 | rounded on write; the constrained ones clamp |
| Vector, VectorDistance, Position | Scalar 3 | |
| Direction | Direction 3 | slerp on the sphere, as the old `interpolate()` |
| Rotation | Quaternion 4 | 3.13 |
| Placement | Scalar 3 + Quaternion 4 | position along the curve, rotation 3.13 |
| Color | Colour 4 | linear in stored sRGB by default (CAD expectation) |
| Material (diffuse, transparency...) | Colour and Scalar groups | through the material's own sub-paths |
| Lists of the above | per element, same size only | different sizes: stepped |
| Bool, Enumeration, String, Link, LinkList, shapes | Step | Visibility steps; a fade uses Transparency |

A stepped key holds the property's value as its own XML text (the type's
`Save`/`Restore`). That covers every property type, even one written in
Python: a Link that swaps its target at 2 s is a step track.

`Property::interpolate(from, to, t)` (in the tree, unused) is now the base
class's, written over the codec; the fourteen per-type overrides are gone.
Where the type has no codec, or the two values' layouts differ (lists of
different sizes), it steps: `from` below one half, `to` from it on.

### 3.13 Rotations

The trap is old and the same everywhere. A quaternion slerp takes the
shortest way, so it cannot turn a part 720 deg between two keys. Euler
angles can, but pass through gimbal lock. A rotation track therefore picks
its mode:

- **Shortest** (default): slerp between keys. Bezier runs on the four
  components and normalises, after each key's quaternion is turned to the
  side of the one before (`q` and `-q` are one rotation), so a span never
  goes round the far side. That is Blender's way. Squad, the true spline on
  the sphere, can replace it if the difference ever shows. Slerp is what
  glTF plays.
- **Turns**: the key's axis-angle is kept unwrapped (an angle of 720 deg is
  two turns), and the angle is interpolated as a scalar about an
  interpolated axis. This is what a "spin the shaft 10 turns" key needs. On
  export it is baked to samples.
- **Euler**: yaw, pitch and roll as three scalars. Blender's default, for
  someone who works that way.

A track on a sub-path (`Placement.Rotation.Angle` with the axis fixed) is a
scalar track and needs none of this.

## 4. Evaluation

`Animation::evaluate(t)`, called whenever `Time` changes:

1. For each track not muted, find the span holding `t` with a binary search
   on the key times. Outside the keys, apply the track's extrapolation.
   Linear goes on along the straight line through the two end keys,
   whatever their interpolation; CycleOffset adds the last key's change over
   the first for scalars and colours, composes it for quaternions, and
   leaves directions cycling.
2. Evaluate the span:
   - Constant takes the left key's value.
   - Linear and Easing compute `u = (t - t0) / (t1 - t0)`, ease it, and
     blend per group: lerp for scalars and colours, slerp for quaternions.
   - Bezier solves the curve's x(s) = t for s, then evaluates y(s) per
     component: Blender's 2D F-curve. Directions and quaternions are
     normalised after (3.13). Handles that reach past each other in time are
     shortened together, so the curve stays a function of time.
   - Python calls the callable with `u`, inside the expression sandbox
     rules.
3. Group the tracks by property. For each property, start from the whole
   track's value if there is one, else from the property's current value.
   Apply its part tracks in path order (3.4), then write the property ONCE
   with `setAnimValue`, or `setPathValue` for a sub-path with no codec,
   inside `App::AnimatedWrites`. One write per property per frame means one
   `onChanged` and one touch, however many tracks it has. A write equal to the current value does nothing,
   by the value-equality short-circuit `hasSetValue` already has.

   Built, phase 2, the parts differ: the whole track is written with
   `setAnimValue`, then each part with `setPathValue`, in path order -- one
   write per track rather than per property. Composing the parts on a copy
   first needs the path's unit rules (an angle in degrees, `Yaw` that a
   Python rotation has no attribute for) applied to a value with no owner;
   `PropertyPlacement::setPathValue()` has them only on the live property.
   A part's value goes through a scratch property of its own type
   (`PropertyQuantity` with the part's unit, `PropertyFloat`,
   `PropertyVector`, `PropertyRotation`, `PropertyPlacement`), so it has the
   same codec as a whole track.
4. Recompute by the `Recompute` mode:
   - Touched (the default) recomputes what the writes touched.
   - Never (the old `NoRecompute`) updates only the view. That is right for
     placements of leaf parts and view properties, and leaves anything
     downstream stale until stop.
   - Full recomputes everything.

   A track on an Assembly joint value runs the solver as part of that
   recompute.

Real time (playing) against every frame (export) is the old
`AutoSkipFrame`. The player steps `Time` by wall-clock time times `Speed`,
and when a frame's recompute overruns, the next frame jumps ahead. Export
sets `Time` to each frame time in turn and waits.

Performance, for large models:
- Evaluating keys costs microseconds.
- Recompute is the cost, and it is what the out-of-process OCCT work
  parallelises.
- A placement change must stay a transform update all the way down:
  Coin node, render cache, bgfx instance. It must not re-tessellate. The
  TShape render cache already shares the tessellation, and this is to be
  measured in phase 2.
- A frame cache (the old `CacheProperties`/`EnableCache`) stores each
  frame's shapes for scrubbing a parametric animation. It is phase 5, and
  only if recompute turns out too slow. It did, and it is built: 4.1.

### 4.1 The frame cache, as built

What it is for: an Animation that drives a parameter costs a recompute a
frame, and a recompute is as slow as the model. A plate whose thickness
is animated, 64 holes cut through it and every edge filleted (218 faces),
takes 290 ms a frame on mini; played a second time it took 290 ms again.
Others keep what a frame evaluated to (Maya's cached playback, Houdini's
cooked geometry) or bake it by hand (Blender's point caches, an Alembic
sequence); the CAD tools keep placements only and render a parameter
offline. Here the document keeps what the recompute wrote.

`Cache` (a property of the Animation, off by default, saved; the frames
are not). With it on, and `Recompute` not Never:

- **A frame is a time on the grid**, `Start + n / FrameRate`
  (`frameAt()`). Any other time is recomputed and not kept. The player
  therefore plays a caching Animation frame by frame: `Time` is set to
  the frame last passed, and not set at all while that frame is on show.
- **Kept**: after the tracks are written, the recompute runs with every
  `signalChangedObject` and `signalRecomputedObject` noted. Each property
  written is copied (`Property::Copy`) -- a shape shares its `TShape`, so
  the copy is a handle, and the mesh the view makes of it afterwards is
  on that same `TShape` and comes with it. With the copies go the frame's
  *scope* (every object touched before the recompute, recomputed by it or
  written by it) and the ones of them it left untouched. A recompute that
  fails, or leaves an object in error, keeps nothing.
- **Put back**: the next time `Time` is that frame the tracks are written
  as ever, and if nothing is touched outside the frame's scope the copies
  are pasted, under the Animation's write cause, and the untouched ones
  purged; no recompute. If something is touched that the frame's
  recompute never met -- the frame was kept coming from the frame before,
  where one object had not changed, and is reached now from a frame where
  it differs -- the frame is recomputed and kept again, wider.
- **Dropped**: by a change of a property that is not the Animation's own
  writing (`WriteCause::Animation`) -- an edit, an undo, a recompute of
  the design -- the frames whose scope holds the object changed or any
  object depending on it, through any link, an expression's hidden ones
  included (`dropFramesReaching`, `getInListEx`); a frame whose recompute
  met none of them is what a recompute would give now, and stays
  (2026-11-14; before, an edit dropped every frame). Every frame, by a new
  or deleted object, a removed property, a change of `Tracks`, `Start`,
  `FrameRate`, `Recompute` or `Cache`. Stopping keeps them: the design it
  recomputes back to is the one they were made from.
- What is kept is a function of the frame and the design. A result that
  depends on how the frame was reached (a solver starting from the last
  pose) is put back as it was first found.

**Memory.** A frame of that plate holds 3.1 MB without a view and 7.1 MB
with one (the meshes), and `Property::getMemSize()` says 15 KB of it: it
counts a shape's geometry by the size of its classes and leaves out the
poles, the pcurves, the mesh and the element map. So a frame is counted
by what the heap grew by between it and the frame kept before it
(`malloc_zone_statistics`, `mallinfo2`, the private bytes on Windows) --
in that interval the document gave up the earlier frame's values, which
the cache holds on to, for this one's. Where that is not known (the frame
before was put back, not kept) the mean of the frames measured is used.
Measured against the heap itself: 433.8 MB counted for 440 MB grown, 61
frames. Two budgets, in `Preferences/Animation`:

| Parameter | Default | |
|---|---|---|
| `CacheLimitMB` | 512 | what the frames in memory may hold |
| `CacheDiskLimitMB` | 4096 | what the frames on disk may; 0 for no disk |
| `CacheDiskAlways` | false | to disk even where recomputing is quicker |
| `CacheMeasure` | true | false counts by `getMemSize()` |

Over the first, the frames shown longest ago go **to disk**: each value as
a save would write it (`App::captureValue`, what the transaction log
keeps a value as, in the binary shape format; no blob store, the frame's
file holds it whole), one file a frame under
`TransientDir/AnimationCache/<name>` (0.55 MB for the 64-hole plate when
it was written as text). Read back
(`App::restoreValue`) it is in memory again and its file stays good, so
letting it go a second time writes nothing. A file does not hold the
mesh, and holding it does not pay (measured 2026-11-14, the user
manual's plate, 36 holes filleted, 134 faces, mini): written with each
face's triangulation (`BinTools_ShapeSet::SetWithTriangles`) a frame read
back took 88 ms and 1.9 MB, without it 80 ms and 0.21 MB, and in the
text form a save writes 85 ms and 0.33 MB. Headless a read-back is 38 ms
of parsing; the rest is the view taking the shape, as it does for a frame
from memory (39 ms the whole), so the mesh made again at the view's
deflection is not where the time goes. The cache writes binary. A frame that is quicker to
recompute than its trip to disk has been costing, a byte, is dropped
instead; so is every frame over the second budget, oldest first, and
every one on disk at a save, whose string table its element maps name by
id. The frame on show is never let go.

**Detail.** A shape just rebuilt is shown coarse first and exact some
frames later (`Render_CoarseTessellation`), a publish leaves shapes out
when its capture budget is spent, and a shape of many faces stands in as
its bounding box. A frame kept or saved in that state is wrong: the cache
would keep the coarse mesh, and a video shows each rebuilt frame rough.
`Render::ExactDetail` (`Renderer.h`) turns all three off while held:
`PartGui::coarseTessellationLevel()` answers -1,
`buildCoarseStandIn()` declines, and the capture budget is not applied.
Held by an active Animation with `Cache` on (`App::Animation::cachingIn`,
asked through `ExactDetail::also`, so before the first frame is built)
and by `freecad.videoexport.export()` for its duration
(`Gui.setExactDetail`, `exactDetail=False` to opt out), which also waits
for a document still filling its views (`Gui.isBuildingVisuals`). Not
held back: what memory pressure orders (the level plan's descents), and
the stream to web viewers, whose ladder is the network's.

Measured, mini, in the GUI, a frame:

| Scene | Recomputed | From memory | From disk |
|---|---|---|---|
| 36 holes (38 faces) | 38 ms | 1.3 ms | |
| 64 holes, filleted (218 faces) | 290 ms | 26 ms | 107 ms |

The timeline draws a line under its ruler, a piece a frame: green for a
frame in memory, blue for one on disk. Python: `anim.Cache`,
`anim.cacheInfo()` (Times, OnDisk, Bytes, DiskBytes, Stored, Replayed,
Spilled, Loaded), `anim.clearCache()`. Tests: `TestAnimation`
(`testCache*`, nine) and `tests/gui/animation-cache.py`.

Not done: a bake command (playing or exporting once is the bake).

## 5. Playing does not edit the design

### 5.1 The write scope

Every property write is classified once, by `Document::writeEffects()`
(`App/WriteScope.h`), from who writes and what is written; the answer is a
set of effects, and each listener reads its own bit of it:

| Effect | Read by |
|---|---|
| `Step` -- opens an undo step when none is open | `Document::_checkTransaction` |
| `Undo` -- is recorded in the open step, and so in the log's row | `Document::onBeforeChangeProperty` |
| `Dirty` -- changes the owner's file; a folder save writes it again | `Document::noteSavedChange` |
| `Modified` -- an edit: `Document::signalEdited`, the one thing a change marks the Gui document modified on | `Document::noteChanged`, `noteTouched` |
| `Key` -- is keyed by Record | `Animation::onBeforeRecord`, `onRecord` |

Who writes is a set of causes. Two are scopes, `App::WriteScope(cause)`:
`Animation`, and `ViewFit` (view state fitted to the model, an origin's
size). The rest is the document's state: `Bookkeeping`, `Transacting` (an
undo, redo or replay), `Restoring`, `Recompute`, `DeferredLoad`. Scopes add
up; a scope of `Edit` starts over, for a real edit made from inside one.
What is written is the property's traits: a view provider's, never saved,
`NoModify`, `Legacy`. The table is `writeEffectsOf()` in
`App/WriteScope.cpp`, and `tests/src/App/WriteScope.cpp` pins it:

| Write | Step | Undo | Dirty | Modified | Key |
|---|---|---|---|---|---|
| an edit of a saved property | x | x | x | x | x |
| `NoModify` | | x | x | | x |
| `Legacy` (object's) | | x | x | x | x |
| never saved (object's) | x | x | | x | x |
| never saved (view's, log on) | | x | | x | x |
| `ViewFit`, a view's property (log on) | | x | x | | x |
| `Bookkeeping` | | | x | x | x |
| `Transacting`, `Restoring`, `Recompute` | x | x | x | x | |
| `Animation` | | | | | |
| `DeferredLoad` | | | | | |
| a touch or a purge | | | | x | |
| a touch or a purge under `Animation` | | | | | |

`Step` and `Undo` under `Transacting` are what the table says of the
write; the document itself opens and records nothing while it puts values
back. `Dirty` under `Restoring` is likewise dropped by `noteSavedChange`.

A touch is no change to the object's file though the touch state is
saved: it is written in the document's own file, among what that says of
each object, which every save writes. Marking it `Dirty` was tried and
made a folder save write every object a recompute had passed
(`FolderSaveCases`).

Under the `Animation` scope, then, a property write:
- opens no transaction and puts nothing on the undo stack;
- is not logged (TransactionLogDesign sec 2.4 gets a line for it);
- does not set `Gui::Document::setModified`;
- is skipped by Record.

The transaction log already handles a write that lands in no transaction:
it forgets what it holds for that property and serialises it afresh at the
next snapshot (sec 2.4).

### 5.2 Rest values

When an Animation becomes active (its `Time` is set, Play, a scrub), it
saves the current value of each target it writes before the first write.
That is its *rest* value, the design's.
- **Stop**, deactivating, and switching to another animation write the rest
  values back (under the scope) and recompute.
- **Save** while active writes the rest values, not the pose of the
  moment. The document is saved as it would be with the animation stopped;
  the pose comes back right after. That is the rule of every CAD tool
  above, and it keeps an animation from quietly moving the design.
- **Undo** while active acts on the design. It does not stop the
  animation: when a transaction first records an animated property, it
  records the rest value as the value before the edit, not the pose on
  show (`Animation::restValueOf()`, called by
  `TransactionObject::setProperty()`). Undoing the edit then puts the
  design's value back, and the redo step it makes records the edit. Built,
  phase 2.

How the rest value follows the design, without a signal per property:
each property keeps, beside its rest value, the value the last evaluation
wrote. At the next evaluation, a property that no longer holds that value
was written by somebody else -- an edit, an undo, a recompute, a script
with no transaction -- and its value now becomes the rest value. A
transaction recording the property clears the written value, since what
follows is the design's. This works the same for a view provider's
property, which App sees no signal for.

A property already recorded in an open transaction and then animated in
the same transaction (a script that edits a placement and sets `Time`
before committing) is logged with its rest value as the value after
(2026-11-14). The commit copies each value after at once (the log's sec
25.4) and now takes `Animation::heldRestValue()` where an Animation shows a
pose -- a lookup that, unlike `restValueOf()`, does not take what comes
next for an edit -- and so does the comparison that drops a write that
changed nothing, and `resolvePending`. Before, the undo stack was right
and the log's row held the pose. Test: `TestAnimation`,
`testTheLogKeepsTheDesignsValueNotThePoseOfAnEditPlayedInItsTransaction`.

A **user edit to an animated property** while the animation is active:
- With Record on, it keys the value at the current time (section 6).
- With Record off, it changes the rest value, the design. It is then
  overwritten in the view at the next evaluation, and the property editor
  shows that property as "changed, not keyed" (Blender's orange).

A property edited while not animated is a plain design edit; nothing
changes.

Opening a file: `Time` and `Active` are saved; an active animation
evaluates once after the document is restored.

## 6. Making keys

- **Insert key** on a property in the property editor's context menu, and
  `I` over it in the editor: keys the current value at `Time`, as a whole
  property. A sub-path row, such as a Placement's Angle, keys that
  sub-path. Delete key and Clear track sit beside it.
- **Key selection**: keys the selected objects' Placement (and, with an
  option, Visibility and Transparency) -- the common CAD case, one
  command.
- **Record** (auto-key; the old `Recording`, the record icon): every change
  made to a property while Record is on keys it at `Time` on the active
  Animation. That is the old capture by posing, with each property keeping
  its own key times. Dragging with the transform dragger keys the Placement
  at the end of the drag, not at every mouse move.

  Where a property has a whole track and part tracks, a change keys the
  whole track, and also each part track whose piece changed, with that
  piece's new value. Without the second, the parts applied after the whole
  would undo the pose just recorded: a box dragged up while `Base.z` has a
  track of its own would drop back to the curve's height. A property with
  part tracks only keys those whose piece changed.

  Skipped:
  - Output, Transient and NoPersist properties;
  - writes that are not `Key` in the table of sec 5.1 (played, written by
    a recompute, put back by an undo);
  - the Animation's `Exclude` list.
- **Key all changed**: keys every property changed since the last key time,
  in one step. That is the old "new key frame" command.

The property editor marks keyed properties: a key at the current time,
between keys, and changed but not keyed. Blender's three colours, taken
from the theme.

## 7. Placing animations in time (phase 4)

The old Trigger/Delay/Jump comes back as *strips*, like the NLA and Fusion's
storyboards. An Animation may hold other Animations as strips.
- Each strip has an offset, a scale and a repeat count, and runs Forward or
  Reverse. Reverse gives Fusion's explode/assemble from one recording.
- Strips blend by Replace or Add (Add adds to the rest value: a vibration
  on top of a motion).
- A strip's start can be After previous, With previous, or On click. On
  click makes a presentation: Next plays to the next click point.

This needs nothing from sections 3 to 6 beyond evaluating a child
Animation at a mapped time.

## 8. The camera

The camera is not a document property, so its track is a Gui-side track
type on the Animation, keyed per 3D view. Each key holds a camera:
- position
- orientation
- focal distance
- height or angle
- projection

`App::SavedView` already stores cameras with view state, so "key the
camera" can key a SavedView's camera and step the rest of its state
(visibilities, clipping, draw style) at the key.

Interpolation: the orientation by slerp (or squad), the position and the
focal point along their own curves. The camera turns about the focal point
rather than cutting across it, as SOLIDWORKS' view orientation keys do. A
camera track plays only in the view it was keyed in, or in the export view.

## 9. Export

- **Video and image sequences.** The Assembly simulation already renders a
  run to video: `CommandCreateSimulation.py` `videoFrame`, `videoLayout`,
  background options, and the playback timing of AssemblyDynamics.md
  sec 8.6. Those move to a shared exporter that takes any time source. A
  simulation run is then one, and an Animation another.
- **glTF animation** for the web viewer and for other tools:
  - placement tracks of leaf objects become node TRS channels;
  - LINEAR and STEP map one to one;
  - Bezier becomes CUBICSPLINE where it can and samples otherwise;
  - easing and Turns are baked to samples at the frame rate.

  Tracks that rebuild geometry do not fit glTF. They export as a sampled
  mesh sequence or not at all, and the exporter says which.
- **Streaming**: the SceneServer sends `Time`, and the web viewer plays
  placement and visibility tracks locally. The rest it receives as scene
  updates.
- **Renderer**: time-animated effects read one shared clock (RenderEngine.md,
  "Determinism"). During export that clock follows `Time`, so water and
  fire match the animation frame for frame.

### 9.1 Streaming, as built

A served document (`Gui.serveDocument`) used to reach its viewers as the
scene updates each frame's writes make: a publish per frame, a kilobyte or
so for every object that moved, at whatever rate the host managed. Now the
viewer plays the motion by itself and the host publishes nothing for it.

**The clip is baked from the feed, not from the tracks.** When the player
starts an Animation of a served document that somebody is watching, the
serve source steps it through its range -- at `FrameRate`, 240 frames at
most -- and after each step traverses the served graph and reads the draws
it would publish (`Render::MotionClipBaker`, `Gui/Renderer/MotionClip.h`).
An object, on this wire, is an `objectKey`: one occurrence of a document
object, as its node path has it. Where every draw of a key moved by one
rigid transform against the first frame, the key gets a track: that
transform, a translation and a quaternion per frame. Nothing in the bake
knows what a track of the Animation is, so keys of any interpolation,
easing, Turns, strips, an expression reading `Time`, a group or a Link
moving its children all come out right with no case for each -- the same
argument as the glTF exporter's baking, one level further down. The time
it was at is put back when the sweep is done.

**What is not a move rides the clip too** (2026-11-14). A key whose draws
are missing at some frames -- hidden -- keeps its track, with a byte a
frame saying where it is shown; its poses are measured from the first
frame it is shown in, and its draws there are kept, so a viewer holding a
publish taken while it was hidden can still show it. A key whose draws do
not move rigidly -- a shape rebuilt, a colour or a transparency changed,
draws moving apart -- is a *sequence*: its draws at every frame are kept,
the frames before the one that showed it being made from the moves they
were. What is kept is a **blob**: the key's draws as a self-contained
scene snapshot (`Render::motionBlobOf`, `Gui/Renderer/MotionClipBlob.h`,
the layout a bundled `.fcsd` has, meshes and materials inline), named by
the SHA-1 of its bytes, so a frame that draws as another is one blob. The
serve source hands the clip's blobs to the scene server
(`SceneStreamServer::holdBlobs`), which keeps them through its publish
generations for as long as the clip lives and serves them as any chunk,
`GET /blob?key=` and the batched fetch. Blobs are counted as they are
made; past `StreamBlobMB` (`BaseApp/Preferences/Animation`, 64 by
default) a sequence that needs more is left out of the clip, and so is a
hidden key's kept draws. The sweep holds `Render::ExactDetail`: a frame is
kept with the exact mesh, not the coarse one coarse-first tessellation or
the capture budget would stand in for a while.

**Two messages on the control channel**, both built and parsed by the one
header the desktop and the browser's viewer share:

    {"cmd":"clip","doc":..,"id":N,"start":s,"step":dt,"frames":F,
     "keys":["<objectKey, hex>",..],"data":"<base64: F x 7 float32 a key>",
     "shown":"<base64: F bytes a key, or empty>","refs":[frame,blob,..],
     "seqKeys":["<objectKey, hex>",..],"seq":"<base64: F int32 a key>",
     "blobs":["<sha1>",..],"sizes":[bytes,..]}
    {"cmd":"time","doc":..,"clip":N,"active":..,"playing":..,"t":..,
     "raw":..,"speed":..,"start":..,"end":..,"loop":..,
     "v":<manifest version>,"baseActive":..,"base":<Time of that publish>}

`time` is the host's clock: sent when the player's state changes, when
`Speed`, `Loop` or the range do, after every publish, and once a second
while it plays. `raw` is the player's own count before `Loop` folds it, so
the viewer folds it with the same arithmetic (`MotionClock::timeAfter` is
`AnimationPlayer::tick`) and a PingPong runs the same way on both sides.

**Every publish is named with the Time its pose was taken at** (`v`,
`base`). That is what makes the rest simple: a viewer holds the draws of
some publish, and to show Time `t` it moves each clip object's draws by
`D(t) D(base)^-1`. It does not matter how old the publish is or whether
more arrive while it plays; a publish that arrives in the middle is posed
from its own Time. Without a name for the publish it holds (the message
not there yet, or no Animation active when it was taken) the viewer draws
it as it came.

**What is held.** With the clip at the viewers, a publish for what the
Animation writes says nothing new. The serve source asks before it
schedules one (`SceneServeClip::changed`), and the answer is yes only for
a write made by the Animation (`WriteCause::Animation`, section 5.1) while
the player plays, the clip being *exact*: the sweep changed nothing in the
feed that the clip does not say -- a track, a shown byte or a sequence's
blob. What the Animation writes at a Time is what it wrote there in the
sweep, so with an exact clip none of it is news, whatever the property (a
moved Part::Feature's `Shape` is written along with its `Placement`, a
rebuilt one's along with its `Height`). Only a clip that left something
out -- the blobs over their limit -- is not exact; then everything the
Animation writes publishes as it always did, and the viewer still plays
what the clip does say between the publishes that bring the rest. Pausing, scrubbing and stopping always publish, so a viewer that is
not playing holds the very pose the host is in.

**A real edit makes the clip stale.** Any change not made by the
Animation, a change of its tracks or range, an object added or removed:
the viewers are told there is no clip, publishes flow, and if it is still
playing the clip is baked again half a second after the edits stop. A
viewer that joins while it plays is sent the clip and the clock; if nobody
was watching when it started, the join is what bakes it.

**In the viewer** (`wasm/main.cpp`) the scene it holds is recomposed from
the publish's draws once a frame, before it is decided whether a frame is
drawn: a track's draws moved, or left out at a frame it is hidden at; a
sequence's draws swapped for those of the blob of the frame nearest, or
left out where it has none; a track the publish lacks drawn from its kept
draws, moved from the frame they were taken at. At the very Time the
publish was taken nothing is changed -- the publish is exact there, which
is what a paused viewer shows. The blobs are asked for when the clip
arrives, through the same store a scene's chunks go through (IndexedDB,
the batched fetch); one not in yet leaves the publish's draws standing.
The selection drawn over them is rebuilt from the posed draws.
`window.fcviewerAnimationState()` says what it is playing: tracks,
sequences, blobs and how many are in, and what the last pose moved,
swapped and left out.

Not in this cut:
- **A sequence is stepped**, shown at the frame nearest, at `FrameRate`
  (240 frames over the range at most), not blended between frames.
- **A blob is self-contained**, so a mesh that only changes colour is in
  every frame's blob again; out-of-band mesh chunks keyed by content
  would share it. The manual's scene (a post rebuilt, a box made
  transparent, 151 frames) takes 301 blobs.
- **Picking while it plays** in the browser is against the draws as the
  viewer has moved them, but the host resolves the pick in the pose its
  own clock is at; the two agree to within a frame, not exactly.
- The desktop's own mirror of a served scene and the Cycles stream do not
  read the clip.
- The clip rides the control channel as base64: a few hundred kilobytes
  for some tens of moving objects, sent once per bake.

## 10. Python

Built, phase 2 (`App/AnimationPy.xml`; tests in `Mod/Test/TestAnimation.py`):

```python
anim = doc.addObject("App::Animation", "Anim")
anim.addKey("Box.Placement", 0.0)                       # the current value; -> key id
anim.addKey("Box.Placement", 2.0, App.Placement(App.Vector(0, 0, 50), App.Rotation()),
            interp="Bezier")
anim.addKey("Box.ViewObject.Transparency", 1.0, 80,
            easing=("Bounce", "Out"))                   # easing makes interp "Easing"
anim.addKey("Pad.Length", 3.0, "40 mm")
anim.addKey("Box.Placement.Base.z", 1.0, 20)            # a part: applied after the whole
anim.setInterp("Box.Placement", 0.0, "Constant")
anim.moveKey("Box.Placement", 2.0, 2.5)
anim.removeKey("Pad.Length", 3.0)
anim.setTrack("Box.Placement", extrapolation=("Constant", "Cycle"),
              rotation="Turns", label="lid", mute=False, lock=False)   # keys converted
anim.track("Box.Placement")      # dict: Id, Path, ValueType, State, Keys (Numbers or Text) ...
anim.keys("Box.Placement")       # [(time, value, interp), ...]
anim.state("Box.Placement")      # 'Ok' | 'Unresolved' | 'Mismatch' | 'Driven' | 'Solved'
anim.Time = 1.5                  # evaluates, and makes this the active Animation
anim.value("Box.Placement", 1.5) # evaluates without writing
anim.frames()                    # [Start, Start + 1/FrameRate, ..., End]
anim.stop()                      # rest values back, Active off
anim.Cache = True                # keep what each frame recomputes to (4.1)
anim.cacheInfo()                 # dict: Times, OnDisk, Bytes, DiskBytes, Stored, Replayed ...
anim.clearCache()
```

The track's options are a method, not attributes of a track object, so a
track has no Python object to keep alive across an undo. `Tracks` reads as
a list of dicts and is not written from Python. Not yet: the `Python`
interpolation (a callable per span), and `Solved` detection, which needs
the Assembly's view of which parts it solves (both phase 3). Stepped keys
hold the property's own XML, which serves every type that saves itself
inline; a type that saves to a file of its own in the archive (a shape)
is not one to step.

And the driver form needs nothing new:
`Gear2.setExpression(".Placement.Rotation.Angle", "-2 * Anim.Time * 30 deg")`.

## 11. Phases

| Phase | Content | Tested by |
|---|---|---|
| 1 | Codec on the property types of 3.12; `interpolate()` over it; easing and Bezier evaluation; rotation modes | C++ unit tests: values at known times, against closed forms |
| 2 | `App::Animation`, `PropertyAnimationTracks` (save, restore, undo of key edits), `AnimatedWrites`, rest values, Python API, `Time` in expressions | Python suite, headless; the transaction log suite stays green; save while active writes the rest values |
| 3 | Gui: a timeline panel (play, pause, stop, previous and next key, scrub, range, frame rate, keys per track), insert key and key markers in the property editor, Record; the old branch's icons and command names | GUI tests under the offscreen and Windows hidden-desktop harnesses |

Phase 3, built (2026-11-12), and what is left of it:
- App: Record (`onBeforeRecord`/`onRecord` on the document's change
  signals: the value before the edit stays the rest value, the edit is a
  pose; a drag is one key, replaced at each move, in the edit's own step),
  `keyNow()` for Insert key with the part tracks whose piece moved,
  `keyState()` for the marks, `nextKeyTime()`. Tests in TestAnimation.
- Gui: `AnimationTimeline` (a dock, `Std_AnimationTimeline`),
  `AnimationPlayer`, `ViewProviderAnimation`, `CommandAnimation.cpp`
  (`Std_AnimationNew`, `Timeline`, `Play`, `Pause`, `Stop`, `Start`,
  `Prev`, `Next`, `Record`, `NewFrame` = key the selection's placements),
  Tools > Animation; the property editor's Insert key / Delete key / Clear
  track and its yellow, green and orange marks (`Gui::animationKeyMark()`).
- A path through `ViewObject` resolves through ObjectIdentifier's pseudo
  property of that name (it answers the object's Label in its place) to
  the view provider's own property.
- The user manual: thundereal-manual `core/animation` (screenshots and two
  videos, made on macOS in a real window) and the help page
  thundereal-help `pages/Animation.md`.
- Then (3d7ec1623a): `tests/gui/animation-timeline.py`, 29 checks; `I`
  over the property editor; a Driven or Solved row's name dark orange;
  Record of view provider properties -- only the property the change was
  made to, not what it set in turn, a Legacy name (Part's Transparency)
  included; `Gui::Document::slotTouchedObject` no longer marks the
  document modified for an animated write's touch.
- Left: the Driven mark inside the expression editor.

Phase 4, built so far (2026-11-12):
- The camera track (4173274215, `Gui/AnimationCamera.cpp`): the handler
  interface of 3.6 now takes the Animation (`read`, `write`,
  `deactivate`) and names new tracks (`newTrackData`). A key is the point
  looked at (S3), the orientation (Q4), the distance and the size (S2):
  the camera stands back from the interpolated point, so it turns about
  it. The data is "View1/ortho": the view by persistent name, and the
  kind of camera the size means. Rest: the camera before the first
  frame, or where the user moved it since (the handler's own written /
  rest pair, the lazy rule of 5.2). Python names a handler's track by its
  kind, "Camera" or "Camera:View1/ortho".
- The curve editor (3edee73b72): the timeline's Curves mode, keys
  dragged in time and value, Bezier handles dragged Free (Alt mirrors).
  Each drag one undo step.
- Strips (41c6e29972): a track of kind "Strip" names the child Animation
  and carries its start (At, After, With, Click), offset, scale, repeat,
  reverse and blend. The Animation evaluates its own tracks, then each
  strip's tracks at the strip's time, by layer, through its own rest
  values. Before its start a strip plays nothing, after its end it holds.
  Add composes on a whole property's track only (scalars added, a turn
  composed, a direction left), onto the rest value when nothing below
  wrote that frame; a part track of an Add strip replaces -- reading a
  piece of a rest copy goes through the live object. The player stops at
  On click starts. A strip playing its parent is refused; nesting stops
  at eight.
- Phase 4 is done. Left for later: Squad for rotations if Bezier on the
  components ever shows, Aligned handles kept in line by the curve
  editor (it makes a dragged handle Free).
| 4 | Bezier handles in a curve editor; camera track; strips (section 7) | |
| 5 | Shared video exporter; glTF export; streaming of `Time`; frame cache (4.1) | |

Phase 5, built so far (2026-11-12):
- The shared exporter, `freecad.videoexport` (`src/Ext/freecad`):
  `export(path, times, show, ...)` takes any time source -- the frame
  times and a callable that brings the document to one -- saves the view
  at each as a PNG (`compose` may paint over it), and encodes them: a
  video (.mp4, .webm, .avi) by PyAV when it is installed and by an ffmpeg
  executable otherwise, a GIF by Pillow or ffmpeg, and a path ending in
  .png keeps the numbered frames. `exportAnimation(anim, path)` is the
  Animation's: every frame of its range at its `FrameRate`, the animation
  left as it was (stopped, or at the time it showed). The Assembly
  simulation's Save Animation is now a caller: its own frame loop and
  encoders are gone, its Results box and time overlay are its `compose`.
- `Std_AnimationExport` (Tools > Animation, and the timeline's bar) asks
  for a file and exports the current Animation.
- glTF, `freecad.animationgltf.exportGltf(anim, path)`: the document's
  shown objects as nodes, nested as the document nests them, meshed as
  the view shows them (`freecad.gltfscene`, the pieces the Assembly's
  exporter had, now shared). **Every channel is baked**: the Animation is
  stepped through its frames and each node's placement written as LINEAR
  samples, a visibility that switches as a STEP scale of 1 or 0 (glTF has
  no visibility to animate). Baking is what makes strips, easing, Turns
  and expression-driven placements come out right with no case for each.
  Then (2026-11-14) the one-to-one mapping of section 9: an object whose
  Placement has a whole track of all Constant, all Linear, or Bezier (and
  Linear) spans gets STEP, LINEAR or CUBICSPLINE channels on the key
  times, the tangents a second-order difference off each side of a key
  (1e-4 s and 2e-4 s). Translation and rotation each take the keys only
  when the channel gives back every baked frame (1 um; 1 - |dot| 1e-9,
  about 1e-4 rad), else that one stays samples -- so a part track, easing,
  Turns, a strip or an expression needs no rule of its own. A Bezier
  rotation blends its control points by slerp, a curve on the sphere a
  CUBICSPLINE on the components does not follow: a 30 deg turn fits, the
  manual lid's 110 deg misses by about 1.3 deg mid-span and is sampled,
  its position keyed (`PartlyKeyed`). A visibility is written where it
  switches only. The same pass found that both exporters began from the
  design at rest: a Time already at the range's start is no change and
  activates nothing, so frame 0 was the rest value wherever the first key
  is not it; each step now activates the Animation. What does not fit is named in the
  result's `Skipped`: an object whose shape the animation rebuilds
  (exported as it rests), a colour or transparency that changes, camera
  tracks.
- Tests: `TestAnimation` (glTF channels and samples read back from the
  file, what is skipped, the formats), `tests/gui/animation-export.py`
  (12 checks: sequence, GIF, video, cancel, the animation left as it
  was, not modified, the command).
- The renderer's clock follows the frame during export (section 9):
  `Render::AnimationClock` pins the clock every time-animated effect
  reads, process-wide; `Gui.setRenderClock(seconds or None)` and
  `Gui.renderClock()` are its Python, and `videoexport.export` pins it to
  each frame's time counted from the first (`renderClock=False` leaves
  it), giving back what it found. A pinned clock asks for no redraw, set
  back it replays the stateful particles from their reset, and the
  volumetric pass replaces its history outright instead of blending the
  frame before in -- all as a frozen frame does. That last one showed in
  the test: without it the first image after any change of the clock was
  a mix of two times, so an export's first frame held what the view had
  drawn last. The Assembly simulation's Save Animation gets it by
  being a caller. Tests: `AnimationClock_tests_run`,
  `tests/gui/animation-export-clock.py` (a flame exported twice: the same
  frames, one by one).
- Streaming of `Time` (section 9.1): the host bakes a motion clip from
  the feed it publishes, sends it with its clock, and holds the publishes
  the clip already says; the browser's viewer plays it by itself.
  `Gui/Renderer/MotionClip.h` (shared), `Gui/SceneServeClip.*` (host),
  `wasm/main.cpp` (viewer). Tests: `MotionClip_tests_run` (the bake, the
  wire, the clock), `tests/gui/serve-animation.py` (the wire, with a
  synthetic client) and `tests/gui/serve-animation-browser.py` (a browser
  drawing it, `scripts/animation-drive.js`). mini has the wasm toolchain
  now (emsdk 6.0.9, `~/works/sw/emsdk`).
- Then (2026-11-14) visibility and everything else in the clip: hidden
  frames on a track, and sequences of per-frame blobs for what is rebuilt
  or recoloured, held by the scene server while the clip lives (9.1). An
  Animation of any properties now plays in the browser with nothing
  published. Tests: `MotionClip_tests_run` 20 (the blob read back
  included), `serve-animation.py` 34 (a rebuilt box and a hidden one,
  every blob served under its key, nothing published; with no room the
  publishes flow), `tests/gui/serve-animation-sequence-browser.py` 18 (a
  red box shrinking drawn smaller as the Time goes on, a green one hidden
  in the publish drawn between 1 s and 2 s only, all on one publish).
- The frame cache -- "if needed", and the first measurement said not:
  the manual's scene (five tracks, a cylinder rebuilt each
  frame, an expression on Time) evaluates in 0.95 ms a frame with
  `Recompute` Touched and 0.47 ms with Never, worst 1.9 ms, on mini; a
  frame at 30 fps has 33 ms. That is a small scene and says nothing of a
  heavy one: a plate whose thickness the Animation drives, holes cut
  through it, takes 21 ms a frame with 16 holes and 77 ms with 64, and
  with every edge filleted 60 ms and 272 ms (mini, in the GUI, the view's
  update included). A second pass over the same frames is no faster --
  nothing was kept. So a parametric animation of a real part plays
  at 4 to 15 frames a second. Built 2026-11-13, with exact detail held
  while frames are kept or saved: section 4.1.
- The manual (thundereal-manual `core/animation`, "Exporting", with
  `export.py` and its media) and the help page have the export.

## 12. Decided, and still open

Decided by the user, 2026-11-12:

1. Rest values: the CAD rule, as section 5 has it.
2. Storage: one property holding every track (3.3).
3. Time in seconds.
4. The timeline is a dock window; any dock can be an overlay, so there is
   nothing to choose. It is not the Animation's task panel (3.10).
5. The name: `App::Animation`.
6. A conflicting track (Driven, Solved) is written and warned in orange,
   never skipped (3.11).
7. With Record off, editing an animated property while active changes its
   rest value; the view shows the animated value again at the next
   evaluation (5.2).
8. Stop puts the rest values back; Pause keeps the pose (3.9).
9. A property may have a whole track and part tracks at once; the parts
   apply after the whole, shorter path first (3.4).

Nothing else is open in the object model.
