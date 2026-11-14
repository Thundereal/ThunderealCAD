"""An Animation exported (docs/Animation.md sec 9), by the exporter the
Assembly simulation shares (freecad.videoexport).

A document with a box, its Placement keyed up 6 mm over 1 s at 10 frames
a second, which keeps it in the view. Claims:
  - an image sequence: a PNG for each of the 11 frames, numbered, of the
    size asked for, the first, the middle and the last three pictures;
  - a GIF of 11 frames (when there is an encoder for one);
  - a video of the size asked for (when there is an encoder for one);
  - a progress callback saying no stops the export, and leaves no file;
  - after each, the animation is not active and the box is where the
    design has it; exported while active at 0.5 s, it is back at 0.5 s;
  - none of it marks the document modified;
  - Std_AnimationExport, its file dialog answered with a .glb, writes a
    binary glTF;
  - path traced (renderer="Cycles", on the CPU): a PNG a frame of the size
    asked for, the box moving, not the raster's pictures; a frame's own
    seed moves the noise, and denoising takes most of it out; left to
    itself the export follows the view, path traced when its shading is
    External, with the view's own samples and device.

Needs the 3D view's pixels: run in a real window or under xvfb.
"""
import os
import time
import traceback
from unittest import mock

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(seconds=0.2):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


def run():
    try:
        from freecad import videoexport

        App.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
            "AutoSaveEnabled", False)
        App.ParamGet("User parameter:BaseApp/Preferences/View").SetBool(
            "UseNavigationAnimations", False)
        Gui.getMainWindow().resize(1300, 900)
        doc = App.newDocument("AnimationExport")
        doc.UndoMode = 1
        box = doc.addObject("Part::Box", "Box")
        doc.recompute()
        anim = doc.addObject("App::Animation", "Anim")
        anim.FrameRate = 10
        anim.addKey("Box.Placement", 0.0)
        anim.addKey("Box.Placement", 1.0, App.Placement(App.Vector(0, 0, 6), App.Rotation()))
        doc.recompute()
        view = Gui.ActiveDocument.ActiveView
        view.viewFront()
        view.fitAll()
        pump(0.5)
        saved = os.path.join(OUT, "export.FCStd")
        doc.saveAs(saved)
        # An App save leaves the Gui's flag alone
        Gui.ActiveDocument.save()
        pump(0.2)

        seq = os.path.join(OUT, "seq.png")
        done = videoexport.exportAnimation(anim, seq, size=(321, 240))
        files = [videoexport.frameName(seq, i) for i in range(11)]
        check("an image sequence: a PNG for each frame",
              done["Frames"] == 11 and all(os.path.isfile(f) for f in files)
              and not os.path.exists(videoexport.frameName(seq, 11)), done)
        first, last = QtGui.QImage(files[0]), QtGui.QImage(files[-1])
        check("of the size asked for, made even",
              (first.width(), first.height()) == (320, 240) and done["Size"] == (320, 240),
              (first.width(), first.height()))
        middle = QtGui.QImage(files[5])
        check("the first, the middle and the last frame differ",
              first != last and first != middle and middle != last)
        check("the animation is stopped and the box back",
              not anim.Active and abs(box.Placement.Base.z) < 1e-9, box.Placement.Base.z)

        known = dict((ext, label) for label, ext in videoexport.formats())
        if ".gif" in known:
            gif = os.path.join(OUT, "anim.gif")
            videoexport.exportAnimation(anim, gif, size=(160, 120))
            reader = QtGui.QImageReader(gif)
            count = 0
            while not reader.read().isNull():
                count += 1
            check("a GIF of 11 frames", count == 11, count)
        else:
            note("SKIP no GIF encoder here")
        video = next((e for e in (".mp4", ".webm", ".avi") if e in known), None)
        if video:
            path = os.path.join(OUT, "anim" + video)
            done = videoexport.exportAnimation(anim, path, size=(320, 240))
            check("a video is written",
                  os.path.isfile(path) and os.path.getsize(path) > 500
                  and done["Encoder"] in ("av", "ffmpeg"),
                  (video, done["Encoder"], os.path.getsize(path) if os.path.isfile(path) else 0))
        else:
            note("SKIP no video encoder here")

        stopped = os.path.join(OUT, "stopped.gif" if ".gif" in known else "stopped.png")
        asked = []

        def progress(index, count):
            asked.append((index, count))
            return index < 3

        try:
            videoexport.exportAnimation(anim, stopped, size=(160, 120), progress=progress)
            check("a progress callback saying no stops the export", False)
        except videoexport.Cancelled:
            check("a progress callback saying no stops the export",
                  asked[-1] == (3, 11) and not os.path.exists(stopped), asked)
        check("and leaves the animation stopped", not anim.Active)

        anim.Time = 0.5
        pump(0.2)
        videoexport.exportAnimation(anim, os.path.join(OUT, "active.png"), size=(160, 120))
        check("exported while active at 0.5 s, it is back at 0.5 s",
              anim.Active and abs(anim.Time - 0.5) < 1e-9
              and abs(box.Placement.Base.z - 3.0) < 1e-6,
              (anim.Active, anim.Time, box.Placement.Base.z))
        anim.stop()
        pump(0.2)
        check("none of it marks the document modified", not Gui.ActiveDocument.Modified)

        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(anim)
        glb = os.path.join(OUT, "anim.glb")
        with mock.patch.object(QtWidgets.QFileDialog, "getSaveFileName",
                               return_value=(glb, "Animated glTF, binary (*.glb)")):
            Gui.runCommand("Std_AnimationExport")
        pump(0.3)
        head = b""
        if os.path.isfile(glb):
            with open(glb, "rb") as f:
                head = f.read(4)
        check("Std_AnimationExport writes a binary glTF", head == b"glTF", head)
        check("still not modified", not Gui.ActiveDocument.Modified)
        # The first frame is the Animation's, not the design at rest: keyed
        # away from where it rests, held there, the Time already at the start
        anim.addKey("Box.Placement", 0.0, App.Placement(App.Vector(0, 0, 6), App.Rotation()))
        anim.Time = 0.0
        anim.stop()
        pump(0.2)
        held = os.path.join(OUT, "held.png")
        idle = (not anim.Active, anim.Time.Value)
        videoexport.exportAnimation(anim, held, size=(160, 120))
        firstHeld = QtGui.QImage(videoexport.frameName(held, 0))
        middleHeld = QtGui.QImage(videoexport.frameName(held, 5))
        check("from a Time already at the start, the first frame is the Animation's",
              idle == (True, 0.0) and not firstHeld.isNull() and firstHeld == middleHeld, idle)

        # --- path traced: the frames by Cycles, as the view would trace them
        anim.addKey("Box.Placement", 0.0, App.Placement())
        anim.Time = 0.0
        anim.stop()
        pump(0.2)
        devices = videoexport.cyclesDevices()
        if check("this build path traces", "CPU" in devices, devices):
            traced = os.path.join(OUT, "traced.png")
            done = videoexport.exportAnimation(
                anim, traced, size=(161, 120), renderer="Cycles",
                cycles={"Samples": 8, "Device": "CPU", "Denoise": False})
            tfiles = [videoexport.frameName(traced, i) for i in range(11)]
            tfirst, tlast = QtGui.QImage(tfiles[0]), QtGui.QImage(tfiles[-1])
            check("path traced: a PNG for each frame, of the size asked for",
                  done["Renderer"] == "Cycles" and done["Frames"] == 11
                  and all(os.path.isfile(f) for f in tfiles)
                  and (tfirst.width(), tfirst.height()) == (160, 120), done)
            check("the box moves in them", not tfirst.isNull() and tfirst != tlast)
            # One session kept through the frames: the last frame is the
            # one a session of its own makes of the same Time and seed
            anim.Time = 1.0
            if not anim.Active:
                anim.activate()
            pump(0.3)
            alone = os.path.join(OUT, "alone.png")
            view.cyclesRender(alone, 160, 120, 8, "CPU", True, False, 10)
            anim.stop()
            pump(0.2)
            a, b = QtGui.QImage(alone), QtGui.QImage(tfiles[-1])
            worst = 0
            for y in range(0, a.height(), 2):
                for x in range(0, a.width(), 2):
                    p, q = a.pixel(x, y), b.pixel(x, y)
                    worst = max(worst, max(abs(((p >> s) & 255) - ((q >> s) & 255))
                                           for s in (0, 8, 16)))
            check("a frame of the kept session is the frame a fresh one makes",
                  not a.isNull() and worst <= 2, worst)
            raster = os.path.join(OUT, "raster.png")
            videoexport.exportAnimation(anim, raster, size=(160, 120), renderer="Raster")
            check("and they are not the raster's frames",
                  QtGui.QImage(videoexport.frameName(raster, 0)) != tfirst)
            check("the animation is stopped and the box back after it",
                  not anim.Active and abs(box.Placement.Base.z) < 1e-9, box.Placement.Base.z)

            # Each frame gets its own seed, so the noise moves with the frames;
            # denoised, two seeds agree far more closely
            def noise(denoise):
                shots = []
                for seed in (0, 1):
                    name = os.path.join(OUT, "seed%d-%d.png" % (seed, int(denoise)))
                    view.cyclesRender(name, 160, 120, 4, "CPU", True, denoise, seed)
                    shots.append(QtGui.QImage(name))
                a, b = shots
                total = 0
                for y in range(a.height()):
                    for x in range(a.width()):
                        p, q = a.pixel(x, y), b.pixel(x, y)
                        total += sum(abs(((p >> s) & 255) - ((q >> s) & 255)) for s in (0, 8, 16))
                return total / float(a.width() * a.height() * 3)

            raw, clean = noise(False), noise(True)
            check("two seeds, two patterns of noise", raw > 0.5, "%.3f" % raw)
            check("denoised, they nearly agree", clean < raw * 0.5, "%.3f, %.3f" % (raw, clean))

            # Left to itself, the export follows the view: path traced when
            # the view is (its shading External), with the view's settings
            check("a view drawing as usual exports its raster",
                  videoexport.viewRenderer(view) == "Raster", view.ShadingType)
            was = view.ShadingType
            view.ShadingType = "External"
            pump(0.5)
            options = videoexport.cyclesOptions(view)
            check("a view shaded by the path tracer exports path traced",
                  videoexport.viewRenderer(view) == "Cycles", view.ShadingType)
            check("with its own samples and device",
                  options["Samples"] == view.Cycles_Samples and options["Device"] in devices,
                  options)
            view.ShadingType = was
            pump(0.5)

        App.closeDocument(doc.Name)
        colourInGltf()
    except Exception:
        note("ABORT\n" + traceback.format_exc())
    note("DONE")
    QtCore.QTimer.singleShot(200, QtWidgets.QApplication.quit)


def colourInGltf():
    """A colour and a transparency that change, in an animated glTF: the
    materials of their own, a baseColorFactor channel each by
    KHR_animation_pointer; what does not change shares its material"""
    import json

    from freecad import animationgltf, gltfscene

    doc = App.newDocument("AnimationGltfColour")
    boxes = {}
    for name, x in (("Paint", 0), ("Glass", 20), ("Plain", 40), ("Plain2", 60)):
        boxes[name] = doc.addObject("Part::Box", name)
        boxes[name].Placement.Base = App.Vector(x, 0, 0)
    doc.recompute()
    boxes["Paint"].ViewObject.ShapeColor = (1.0, 0.0, 0.0)
    anim = doc.addObject("App::Animation", "Anim")
    anim.FrameRate = 10
    anim.addKey("Paint.ViewObject.ShapeColor", 0.0, (1.0, 0.0, 0.0))
    anim.addKey("Paint.ViewObject.ShapeColor", 1.0, (0.0, 0.0, 1.0))
    anim.addKey("Glass.ViewObject.Transparency", 0.0, 0)
    anim.addKey("Glass.ViewObject.Transparency", 1.0, 60)
    doc.recompute()
    pump(0.3)
    path = os.path.join(OUT, "colour.gltf")
    done = animationgltf.exportGltf(anim, path)
    with open(path, encoding="utf-8") as f:
        gltf = json.load(f)
    check("the colour and the transparency are animated, nothing skipped for them",
          done["Recoloured"] == 2 and not done["Skipped"], done)
    check("by KHR_animation_pointer, used and not required",
          "KHR_animation_pointer" in gltf.get("extensionsUsed", [])
          and "KHR_animation_pointer" not in gltf.get("extensionsRequired", []),
          gltf.get("extensionsUsed"))
    pointers = {}
    for ch in gltf["animations"][0]["channels"]:
        ext = ch["target"].get("extensions", {}).get("KHR_animation_pointer")
        if ext:
            pointers[ext["pointer"]] = gltf["animations"][0]["samplers"][ch["sampler"]]
    nodes = {n.get("extras", {}).get("name"): n for n in gltf["nodes"]}

    def materials(name):
        mesh = gltf["meshes"][nodes[name]["mesh"]]
        return sorted({p["material"] for p in mesh["primitives"]})

    def values(sampler):
        acc = gltf["accessors"][sampler["output"]]
        view = gltf["bufferViews"][acc["bufferView"]]
        with open(os.path.splitext(path)[0] + ".bin", "rb") as f:
            f.seek(view.get("byteOffset", 0))
            raw = f.read(view["byteLength"])
        import struct

        flat = struct.unpack("<%df" % (len(raw) // 4), raw)
        return [flat[i:i + 4] for i in range(0, len(flat), 4)]

    paint, glass = materials("Paint"), materials("Glass")
    target = lambda m: "/materials/%d/pbrMetallicRoughness/baseColorFactor" % m
    check("each animated box a material of its own, with its channel",
          len(paint) == 1 and len(glass) == 1 and paint != glass
          and target(paint[0]) in pointers and target(glass[0]) in pointers,
          (paint, glass, sorted(pointers)))
    if target(paint[0]) in pointers:
        v = values(pointers[target(paint[0])])
        red, blue = gltfscene.baseColor((1, 0, 0), 0), gltfscene.baseColor((0, 0, 1), 0)
        check("red at the first frame, blue at the last, a sample a frame",
              len(v) == 11 and all(abs(a - b) < 1e-5 for a, b in zip(v[0], red))
              and all(abs(a - b) < 1e-5 for a, b in zip(v[-1], blue)), (v[0], v[-1]))
    if target(glass[0]) in pointers:
        v = values(pointers[target(glass[0])])
        mat = gltf["materials"][glass[0]]
        check("the glass from opaque to 0.4, blended throughout",
              abs(v[0][3] - 1.0) < 1e-6 and abs(v[-1][3] - 0.4) < 1e-5
              and mat.get("alphaMode") == "BLEND", (v[0][3], v[-1][3], mat.get("alphaMode")))
    check("the two that do not change share one material, no channel",
          materials("Plain") == materials("Plain2")
          and target(materials("Plain")[0]) not in pointers,
          (materials("Plain"), materials("Plain2")))
    App.closeDocument(doc.Name)


QtCore.QTimer.singleShot(1000, run)
