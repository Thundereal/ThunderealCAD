# Animation -- key frames for any property

Status: design, nothing built (2026-11-12). Branch `Animation`, from `main`
3dd48e3f6d.

The goal: animate any property of any object -- a placement, a pad's
length, a joint's angle, a colour, a transparency, the camera -- with key
frames and a choice of interpolation per key, play it in the 3D view, scrub
it, record it by posing the model, and export it as video or to the web
viewer. One mechanism, not one per workbench.

## 0. Summary of the decisions proposed

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
   plays are written in a scope (`App::AnimatedWrites`, the pattern of
   `App::DerivedViewWrites`) that opens no transaction, is not logged, and
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

The rest of the document is why, and how.

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

## 3. The model

### 3.1 Objects

```
App::Animation                  (DocumentObject; Gui::ViewProviderAnimation)
  Tracks     PropertyAnimationTracks   every track and key (3.3)
  Time       PropertyTime   s   current time; Output, NoModify, not logged
  Start, End PropertyTime   s   play range; End defaults to the last key
  FrameRate  PropertyFloat      30; playback in real time drops frames,
                                export renders every one
  Loop       Enumeration        Once, Loop, PingPong
  Speed      PropertyFloat      1.0
  Recompute  Enumeration        Touched (default), Never (view only), Full
  Active     Bool               evaluated when Time changes (5.2)
  Exclude    StringList         paths Record never keys (old ExcludeProperties)
```

A document may hold several Animations, like Fusion's storyboards or
SOLIDWORKS' studies. One is active per document at a time, and switching
puts the displaced values back (5.2). Strips that place animations inside
another are section 7.

### 3.2 Track

```
struct Track {
    ObjectIdentifier target;   // Box.Placement, Box.Placement.Base.z,
                               // Box.ViewObject.Transparency, Pad.Length
    std::string label;         // optional, shown in the timeline
    bool mute = false;         // not evaluated
    bool lock = false;         // keys not editable
    Extrap before, after;      // Constant | Linear | Cycle | CycleOffset | PingPong
    RotationMode rotation;     // Shortest | Turns | Euler (3.5), rotation tracks only
    std::vector<Key> keys;     // sorted by time, one key per time
};
```

**Addressing by `ObjectIdentifier`** gives these for free, all of them
already used by expressions:

- renames: the relabel and rename handling expressions get;
- sub-paths: `Placement.Rotation.Angle`, `Placement.Base.z`,
  `Constraints.Width`, a spreadsheet cell `Sheet.A1`;
- view properties through the `ViewObject` pseudo-path
  (`ObjectIdentifier.cpp:1567`);
- other documents.

Resolving uses `getPathValue`/`setPathValue`. For a whole property, a track
writes through the property itself, so the property's own type decides how
it interpolates; that is the fast path. For a sub-path, the track's values
are of the sub-path's type: a number or a `Quantity`.

**A track adds no dependency edge.** Its target path is held the way a
hidden reference is (`hiddenref()`), so an expression that reads
`Anim.Time` from a target the Animation drives is not a cycle. The
Animation is not recomputed; it writes.

**An expression wins over a track.** A property bound to an expression is
not keyed: inserting a key there is refused and Record skips it. To drive
such a property over time, write the expression in terms of `Anim.Time`.
SOLIDWORKS and Inventor do the same with a driven dimension.

### 3.3 Key and storage

```
struct Key {
    double time;
    Value value;               // 3.4
    Interp interp;             // Constant | Linear | Bezier | Easing | Python
    Easing easing;             // type (Sine..Bounce), mode (In|Out|InOut|OutIn),
                               // amplitude, period, overshoot (old defaults 1, 0.3, 1.70158)
    Handle left, right;        // Bezier: type AutoClamped|Auto|Vector|Aligned|Free,
                               // offsets (dt, dv[]) per component
    std::string python;        // Python: a callable's dotted name or expression in t
};
```

`interp` and its parameters govern the span from this key to the next, as
in Blender. That is also how SOLIDWORKS sets a mode on a key point.

`PropertyAnimationTracks` holds `std::vector<Track>`.
- It saves as XML inside the Animation, one element per track and key.
- Editing it goes through its API (add, move or delete keys, set
  interpolation), which wraps `aboutToSetValue`/`hasSetValue`. So key edits
  are ordinary undoable transactions, logged as one property's value.
- `copyBeforeChange` is a copy of the vector; at thousands of keys that is
  well under a millisecond.
- The transaction log sees one value. It could later store tracks as rows
  of their own if large animations make that worthwhile.

### 3.4 Values: how a property type is animated

The animation codec is added to `App::Property`:

```
struct AnimLayout {                 // what the numbers are
    enum Kind { Scalar, Angle, Quaternion, Colour, Step };
    std::vector<std::pair<Kind,int>> groups;   // e.g. Placement: {Scalar,3},{Quaternion,4}
};
virtual bool getAnimLayout(AnimLayout &) const { return false; }   // false: stepped
virtual void getAnimValue(std::vector<double> &) const;
virtual void setAnimValue(const std::vector<double> &);
```

| Property | Layout | Notes |
|---|---|---|
| Float, Quantity (Length, Angle, Percent...) | Scalar 1 (Angle for angles) | Constraint and bounds are applied when the value is set |
| Integer | Scalar 1 | rounded on write |
| Vector, VectorDistance, Position | Scalar 3 | |
| Direction | Quaternion-free: slerp on the sphere | as the old `interpolate()` |
| Rotation | Quaternion 4 | 3.5 |
| Placement | Scalar 3 + Quaternion 4 | position along the curve, rotation 3.5 |
| Color | Colour 4 | linear in stored sRGB by default (CAD expectation) |
| Material (diffuse, transparency...) | Colour and Scalar groups | through the material's own sub-paths |
| Lists of the above | per element, same size only | different sizes: stepped |
| Bool, Enumeration, String, Link, LinkList, shapes | Step | Visibility steps; a fade uses Transparency |

A stepped key holds the property's value as its own XML text (the type's
`Save`/`Restore`). That covers every property type, even one written in
Python: a Link that swaps its target at 2 s is a step track.

`Property::interpolate(from, to, t)` (in the tree, unused) becomes a
default written over the codec, so whatever calls it later still works.

### 3.5 Rotations

The trap is old and the same everywhere. A quaternion slerp takes the
shortest way, so it cannot turn a part 720 deg between two keys. Euler
angles can, but pass through gimbal lock. A rotation track therefore picks
its mode:

- **Shortest** (default): slerp between keys. Bezier uses squad, its
  quaternion equivalent, through the neighbouring keys. This is what glTF
  plays.
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
2. Evaluate the span:
   - Constant takes the left key's value.
   - Linear and Easing compute `u = (t - t0) / (t1 - t0)`, ease it, and
     blend per group: lerp for scalars and colours, slerp for quaternions.
   - Bezier solves the curve's x(s) = t for s, then evaluates y(s) per
     component: Blender's 2D F-curve. Quaternions use squad.
   - Python calls the callable with `u`, inside the expression sandbox
     rules.
3. Write the value with `setAnimValue` or `setPathValue`, inside
   `App::AnimatedWrites`. A write equal to the current value does nothing,
   by the value-equality short-circuit `hasSetValue` already has.
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
  only if recompute turns out too slow.

## 5. Playing does not edit the design

### 5.1 The write scope

`App::AnimatedWrites` is a guard like `App::DerivedViewWrites`
(`AutoTransaction.h:103`). Under it, a property write:
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
- **Undo** while active stops the animation first, so undo acts on the
  design.

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
  at the end of the drag, not at every mouse move. Skipped:
  - Output, Transient and NoPersist properties;
  - writes under `AnimatedWrites` and `DerivedViewWrites`;
  - properties bound to an expression;
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

## 10. Python

```python
anim = doc.addObject("App::Animation", "Anim")
anim.addKey("Box.Placement", 0.0)                       # the current value
anim.addKey("Box.Placement", 2.0, App.Placement(App.Vector(0, 0, 50), App.Rotation()),
            interp="Bezier")
anim.addKey("Box.ViewObject.Transparency", 1.0, 80, interp="Easing",
            easing=("Bounce", "Out"))
anim.addKey("Pad.Length", 3.0, "40 mm")
anim.setInterp("Box.Placement", 0.0, "Constant")
anim.track("Box.Placement").Extrapolation = ("Constant", "Cycle")
anim.Time = 1.5                                         # evaluates
anim.value("Box.Placement", 1.5)                        # evaluate without writing
anim.stop()                                             # rest values back
```

And the driver form needs nothing new:
`Gear2.setExpression(".Placement.Rotation.Angle", "-2 * Anim.Time * 30 deg")`.

## 11. Phases

| Phase | Content | Tested by |
|---|---|---|
| 1 | Codec on the property types of 3.4; `interpolate()` over it; easing and Bezier evaluation; rotation modes | C++ unit tests: values at known times, against closed forms |
| 2 | `App::Animation`, `PropertyAnimationTracks` (save, restore, undo of key edits), `AnimatedWrites`, rest values, Python API, `Time` in expressions | Python suite, headless; the transaction log suite stays green; save while active writes the rest values |
| 3 | Gui: a timeline panel (play, pause, stop, previous and next key, scrub, range, frame rate, keys per track), insert key and key markers in the property editor, Record; the old branch's icons and command names | GUI tests under the offscreen and Windows hidden-desktop harnesses |
| 4 | Bezier handles in a curve editor; camera track; strips (section 7) | |
| 5 | Shared video exporter; glTF export; streaming of `Time`; frame cache if needed | |

## 12. Open, for the user to decide

1. **Rest values (section 5).** CAD's rule (proposed): playing never edits
   the design and a save writes the rest values. Or Blender's: the
   evaluated value is the value, saved as it stands.
2. **Storage (3.3).** One property holding every track (proposed), or the
   old object per key frame, which shows keys in the tree but costs a
   transaction per key on large animations.
3. **Time in seconds (proposed) or in frames.**
4. **Where the timeline lives.** A dock panel under the 3D view (proposed),
   or an overlay panel like the fork's other overlays.
5. **The name.** `App::Animation` as before. Tracks and keys need no
   document objects of their own.
