# SPDX-License-Identifier: LGPL-2.1-or-later
"""A run of the 3D view over time, saved as a video, a GIF or an image
sequence (docs/Animation.md sec 9).

The exporter takes any time source: a list of frame times and a callable
that brings the document to one. An Assembly simulation's run is one
(CommandCreateSimulation.saveAnimation), an App::Animation another
(exportAnimation() here).

    from freecad import videoexport
    videoexport.export("out.mp4", times, show, fps=30)
    videoexport.exportAnimation(anim, "out.mp4")

Each frame is the view's image (View3DInventorPy.saveImage) written as a
PNG, which `compose` may paint over; the frames are then encoded. A video
is encoded by PyAV when it is installed and by an ffmpeg executable
otherwise -- the one beside this Python, or on the PATH; a GIF by Pillow,
or by ffmpeg. A path ending in .png keeps the frames themselves, numbered
(out_00000.png ...), and needs neither.
"""

import os
import shutil
import subprocess
import sys
import tempfile

import FreeCAD as App

__all__ = [
    "Cancelled",
    "cyclesDevices",
    "cyclesOptions",
    "encode",
    "export",
    "exportAnimation",
    "exportAnimationDialog",
    "formats",
    "frameName",
    "videoEncoder",
    "viewRenderer",
]

VIDEO = (".mp4", ".avi", ".webm")

# The codecs of each container, the first one the encoder has
CODECS = {
    ".mp4": ("libx264", "mpeg4"),
    ".avi": ("mpeg4",),
    ".webm": ("libvpx-vp9",),
}


class Cancelled(Exception):
    """The progress callback said to stop"""


def _ffmpeg():
    """The ffmpeg beside this Python (a conda environment's), else on PATH"""
    here = os.path.join(os.path.dirname(sys.executable), "ffmpeg")
    for exe in (here, here + ".exe", shutil.which("ffmpeg") or ""):
        if exe and os.path.isfile(exe):
            return exe
    return None


_ffmpegEncoders = None


def _ffmpegHas(codec):
    global _ffmpegEncoders
    if _ffmpegEncoders is None:
        _ffmpegEncoders = ""
        exe = _ffmpeg()
        if exe:
            try:
                _ffmpegEncoders = subprocess.run(
                    [exe, "-hide_banner", "-encoders"],
                    capture_output=True,
                    text=True,
                    timeout=30,
                    check=False,
                ).stdout
            except (OSError, subprocess.SubprocessError):
                pass
    return (" " + codec + " ") in _ffmpegEncoders


def _av():
    try:
        import av

        return av
    except ImportError:
        return None


def videoEncoder():
    """What encodes a video here: "av", "ffmpeg" or None"""
    if _av() is not None:
        return "av"
    if _ffmpeg():
        return "ffmpeg"
    return None


def _codec(ext):
    """The codec for a container with the encoder there is, None if none"""
    av = _av()
    for codec in CODECS.get(ext, ()):
        if av is not None:
            if codec in av.codecs_available:
                return codec
        elif _ffmpegHas(codec):
            return codec
    return None


def _gifEncoder():
    try:
        from PIL import Image  # noqa: F401

        return "pil"
    except ImportError:
        return "ffmpeg" if _ffmpeg() else None


def formats():
    """[(label, extension)] of what can be written here, a video first"""
    out = []
    for label, ext in (("MP4 Video", ".mp4"), ("WebM Video", ".webm"), ("AVI Video", ".avi")):
        if _codec(ext):
            out.append((label, ext))
    if _gifEncoder():
        out.insert(min(1, len(out)), ("Animated GIF", ".gif"))
    out.append(("PNG image sequence", ".png"))
    return out


def frameName(path, index):
    """The file of frame `index` of an image sequence saved as `path`"""
    stem, ext = os.path.splitext(path)
    return "%s_%05d%s" % (stem, index, ext)


def _encodeAv(path, files, fps, size, codec):
    av = _av()
    from PIL import Image
    import numpy as np

    from fractions import Fraction

    # PyAV takes the rate as a rational; a FrameRate is a float
    rate = Fraction(fps).limit_denominator(1001)
    with av.open(path, "w") as output:
        stream = output.add_stream(codec, rate)
        stream.width, stream.height = size
        stream.pix_fmt = "yuv420p"
        for name in files:
            img = Image.open(name).convert("RGB")
            frame = av.VideoFrame.from_ndarray(np.array(img), format="rgb24")
            output.mux(stream.encode(frame))
        output.mux(stream.encode(None))


def _encodeFfmpeg(path, pattern, fps, codec, gif=False):
    cmd = [_ffmpeg(), "-y", "-loglevel", "error", "-framerate", str(fps), "-i", pattern]
    if gif:
        cmd += ["-loop", "0"]
    else:
        cmd += ["-c:v", codec, "-pix_fmt", "yuv420p"]
        if codec == "libvpx-vp9":
            cmd += ["-b:v", "0", "-crf", "32"]
    cmd.append(path)
    done = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if done.returncode != 0:
        raise RuntimeError("ffmpeg failed: " + (done.stderr.strip() or str(done.returncode)))


def _encodeGif(path, files, _pattern, fps):
    from PIL import Image

    images = [Image.open(f) for f in files]
    images[0].save(
        path,
        save_all=True,
        append_images=images[1:],
        optimize=True,
        duration=int(round(1000.0 / fps)),
        loop=0,  # for ever
    )


def encode(path, files, fps, size=None):
    """Encode the image files, in order, as path: a video (.mp4, .avi,
    .webm) or a GIF. size is a video's (width, height), the images' own by
    default. -> what encoded it: "av", "ffmpeg" or "pil" """
    ext = os.path.splitext(path)[1].lower()
    files = list(files)
    if not files:
        raise ValueError("No frames to encode")
    codec = None
    if ext in VIDEO:
        codec = _codec(ext)
        if not codec:
            raise RuntimeError("No encoder for %s: install PyAV, or put ffmpeg on the PATH" % ext)
        encoder = videoEncoder()
    elif ext == ".gif":
        encoder = _gifEncoder()
        if not encoder:
            raise RuntimeError("No encoder for .gif: install Pillow, or put ffmpeg on the PATH")
    else:
        raise ValueError("Unknown export format '%s'" % ext)

    if encoder == "pil":
        _encodeGif(path, files, None, fps)
        return encoder
    if encoder == "av":
        if size is None:
            from PIL import Image

            size = Image.open(files[0]).size
        _encodeAv(path, files, fps, size, codec)
        return encoder
    # ffmpeg reads a numbered sequence: the files as they are when they are
    # one, else links to them (copies where links are not to be had)
    folder = os.path.dirname(files[0])
    temp = None
    if any(f != os.path.join(folder, "frame_%05d.png" % i) for i, f in enumerate(files)):
        temp = folder = tempfile.mkdtemp(prefix="fc-video-")
        for i, f in enumerate(files):
            name = os.path.join(folder, "frame_%05d.png" % i)
            try:
                os.symlink(os.path.abspath(f), name)
            except OSError:
                shutil.copyfile(f, name)
    try:
        _encodeFfmpeg(path, os.path.join(folder, "frame_%05d.png"), fps, codec, gif=ext == ".gif")
    finally:
        if temp:
            shutil.rmtree(temp, ignore_errors=True)
    return encoder


def _waitLoaded(Gui, timeout=600.0):
    """Lets a document that is still filling its views finish (a
    progressive load): its first frames would lack what has not arrived"""
    import time

    if not hasattr(Gui, "isBuildingVisuals"):
        return
    end = time.monotonic() + timeout
    while Gui.isBuildingVisuals() and time.monotonic() < end:
        Gui.updateGui()
        time.sleep(0.01)


def export(
    path,
    times,
    show,
    view=None,
    size=None,
    fps=30,
    background="Current",
    compose=None,
    progress=None,
    frameSize=None,
    renderClock=True,
    exactDetail=True,
    renderer="Raster",
    cycles=None,
):
    """Save the view over `times` as `path`.

    path        .mp4, .avi, .webm, .gif, or .png for an image sequence
    times       the frame times, in order
    show        show(index, time): brings the document to that time
    view        the 3D view; the active one by default
    size        (width, height) of the view's image; the view's own size by
                default. Made even, as the codecs want
    fps         frames a second of the file
    background  as View3DInventorPy.saveImage() takes it: "Current",
                "White", "Black", "Transparent" or a colour's name
    compose     compose(file, index, time): paints over the frame's PNG, in
                place; it may change its size, to frameSize
    progress    progress(index, count) before each frame, and with index
                == count before encoding; False from it cancels (Cancelled)
    frameSize   the size compose leaves a frame at, if not the view's
    renderClock the renderer's time-animated effects (water, fire,
                particles) are drawn at each frame's time, counted from
                the first; False leaves them on the wall clock
    exactDetail every shape is in a frame with its exact mesh: none coarse
                because it was just rebuilt, none standing in as a box,
                and a document still loading is waited for. False takes
                the view as it draws
    renderer    "Raster", the view's image as it draws it, or "Cycles": each
                frame path traced (View3DInventorPy.cyclesRender), over the
                view's own background -- `background` is the raster's
    cycles      for "Cycles": {"Samples": 64, "Device": "CPU", "Denoise":
                False}, any left out taken from cyclesOptions(view)

    -> {"Path", "Frames", "Size", "Encoder", "Renderer"}; for a sequence
    Path is its first frame.
    """
    import FreeCADGui as Gui

    times = list(times)
    if not times:
        raise ValueError("No frames to export")
    ext = os.path.splitext(path)[1].lower()
    sequence = ext == ".png"
    if ext in VIDEO:
        if not _codec(ext):
            raise RuntimeError("No encoder for %s: install PyAV, or put ffmpeg on the PATH" % ext)
    elif ext == ".gif":
        if not _gifEncoder():
            raise RuntimeError("No encoder for .gif: install Pillow, or put ffmpeg on the PATH")
    elif not sequence:
        raise ValueError("Unknown export format '%s'" % ext)

    if view is None:
        view = Gui.ActiveDocument.ActiveView
    if renderer not in ("Raster", "Cycles"):
        raise ValueError("Unknown renderer '%s': Raster or Cycles" % renderer)
    traced = None
    if renderer == "Cycles":
        if not hasattr(view, "cyclesRender") or not cyclesDevices():
            raise RuntimeError("This build has no Cycles engine to render with")
        traced = cyclesOptions(view)
        traced.update(cycles or {})
    width, height = size if size else view.getSize()
    width -= width % 2
    height -= height % 2
    if width <= 0 or height <= 0:
        raise ValueError("The view has no size to export")
    outSize = tuple(frameSize) if frameSize else (width, height)

    count = len(times)
    temp = None if sequence else tempfile.mkdtemp(prefix="fc-video-")
    # The renderer's time-animated effects (water, fire, particles) read a
    # clock of their own: pinned to each frame's time, they are in the file
    # as they are at that frame, however long the frame took to save
    pinned = Gui.renderClock() if hasattr(Gui, "renderClock") else None
    # A shape the frame's step rebuilt would be shown coarse first and
    # exact some frames later: held exact, it is meshed once, as it is saved
    exact = exactDetail and hasattr(Gui, "setExactDetail")
    detail = Gui.setExactDetail(True) if exact else False
    try:
        if exact:
            _waitLoaded(Gui)
        files = []
        for i, t in enumerate(times):
            if progress is not None and progress(i, count) is False:
                raise Cancelled()
            if renderClock and hasattr(Gui, "setRenderClock"):
                Gui.setRenderClock(float(t) - float(times[0]))
            show(i, t)
            Gui.updateGui()
            name = frameName(path, i) if sequence else os.path.join(temp, "frame_%05d.png" % i)
            if traced is None:
                view.saveImage(name, width, height, background)
            else:
                # A seed a frame: with one seed the noise would stand still
                # on the screen while the scene moves under it. One session
                # through the frames: its device, kernels and shaders are
                # set up once, the scene restated in place
                view.cyclesRender(
                    name,
                    width,
                    height,
                    int(traced["Samples"]),
                    str(traced["Device"]),
                    True,
                    bool(traced["Denoise"]),
                    i,
                    True,
                )
            if compose is not None:
                compose(name, i, t)
            files.append(name)
        if progress is not None and progress(count, count) is False:
            raise Cancelled()

        encoder = "png" if sequence else encode(path, files, fps, outSize)
        return {
            "Path": files[0] if sequence else path,
            "Frames": count,
            "Size": outSize,
            "Encoder": encoder,
            "Renderer": renderer,
        }
    finally:
        if traced is not None and hasattr(view, "cyclesRelease"):
            view.cyclesRelease()
        if renderClock and hasattr(Gui, "setRenderClock"):
            Gui.setRenderClock(pinned)
        if exact:
            Gui.setExactDetail(detail)
        if temp:
            shutil.rmtree(temp, ignore_errors=True)


def cyclesDevices():
    """The device types Cycles can render on here ("CPU", "METAL", "CUDA",
    ...); empty in a build without the engine"""
    import FreeCADGui as Gui

    if not hasattr(Gui, "cyclesDevices"):
        return []
    return [d["type"] for d in Gui.cyclesDevices()]


def _viewProperty(view, name, default):
    try:
        return getattr(view, name)
    except Exception:  # pylint: disable=broad-except
        return default


def cyclesOptions(view):
    """The view's own Cycles settings, as its live path tracing uses them:
    {"Samples", "Device", "Denoise"}"""
    devices = cyclesDevices()
    device = str(_viewProperty(view, "Cycles_Device", "") or "")
    if device not in devices:
        device = "CPU" if "CPU" in devices or not devices else devices[0]
    return {
        "Samples": int(_viewProperty(view, "Cycles_Samples", 64) or 64),
        "Device": device,
        "Denoise": bool(_viewProperty(view, "Cycles_Denoise", False)),
    }


def viewRenderer(view):
    """What the view draws with: "Cycles" when its shading is the path
    tracer (External) and the build has it, else "Raster" """
    if str(_viewProperty(view, "ShadingType", "")) == "External" and cyclesDevices():
        return "Cycles"
    return "Raster"


def exportAnimation(
    anim,
    path,
    view=None,
    size=None,
    background="Current",
    progress=None,
    renderer=None,
    cycles=None,
):
    """Save an App::Animation, each frame of its range at its FrameRate, as
    export() does. The animation is left as it was: stopped if it was not
    active, at the time it showed if it was.

    renderer    "Raster" or "Cycles", as export() takes it; None follows the
                view: path traced when it is (its shading External), with
                its own Cycles settings"""
    import FreeCADGui as Gui

    if view is None:
        view = Gui.ActiveDocument.ActiveView
    if renderer is None:
        renderer = viewRenderer(view)
    wasActive = bool(anim.Active)
    time = anim.Time

    def show(_index, t):
        anim.Time = t
        # A Time already at t is no change, and activates nothing: the
        # first frame would be the design as it rests
        if not anim.Active:
            anim.activate()

    try:
        return export(
            path,
            anim.frames(),
            show,
            view=view,
            size=size,
            fps=anim.FrameRate,
            background=background,
            progress=progress,
            renderer=renderer,
            cycles=cycles,
        )
    finally:
        if wasActive:
            anim.Time = time
        else:
            anim.stop()


def _activeAnimation():
    """The Animation to export: the one selected, else the active one, else
    the document's only one"""
    import FreeCADGui as Gui

    doc = App.ActiveDocument
    if doc is None:
        return None
    for obj in Gui.Selection.getSelection():
        if obj.isDerivedFrom("App::Animation"):
            return obj
    anims = [o for o in doc.Objects if o.isDerivedFrom("App::Animation")]
    for anim in anims:
        if anim.Active:
            return anim
    return anims[0] if len(anims) == 1 else None


def exportAnimationDialog(anim=None):
    """Ask for a file and save the Animation to it, with a progress dialog.
    -> what export() returns, or None when cancelled or there is nothing
    to export"""
    import FreeCADGui as Gui
    from PySide import QtCore, QtWidgets

    translate = App.Qt.translate
    main = Gui.getMainWindow()
    title = translate("Animation", "Export animation")
    if anim is None:
        anim = _activeAnimation()
    if anim is None:
        QtWidgets.QMessageBox.warning(
            main, title, translate("Animation", "Select the Animation to export.")
        )
        return None
    view = Gui.ActiveDocument.ActiveView if Gui.ActiveDocument else None
    if view is None or not hasattr(view, "saveImage"):
        QtWidgets.QMessageBox.warning(
            main, title, translate("Animation", "Make a 3D view active to export it.")
        )
        return None
    known = formats() + [("Animated glTF, binary", ".glb"), ("Animated glTF", ".gltf")]
    path, chosen = QtWidgets.QFileDialog.getSaveFileName(
        main, title, "", ";;".join("%s (*%s)" % f for f in known)
    )
    if not path:
        return None
    if not os.path.splitext(path)[1]:
        path += [ext for label, ext in known if chosen.startswith(label)][0]
    if os.path.splitext(path)[1].lower() in (".glb", ".gltf"):
        from freecad import animationgltf

        QtWidgets.QApplication.setOverrideCursor(QtCore.Qt.WaitCursor)
        try:
            done = animationgltf.exportGltf(anim, path)
        except Exception as e:  # pylint: disable=broad-except
            QtWidgets.QApplication.restoreOverrideCursor()
            QtWidgets.QMessageBox.critical(main, title, str(e))
            return None
        QtWidgets.QApplication.restoreOverrideCursor()
        App.Console.PrintMessage(
            "Animation saved to %s: %d of %d nodes animated, %d frames\n"
            % (path, done["Animated"], done["Nodes"], done["Frames"])
        )
        for line in done["Skipped"]:
            App.Console.PrintWarning("Not in the glTF: %s\n" % line)
        return done

    renderer = viewRenderer(view)
    dialog = QtWidgets.QProgressDialog(
        translate("Animation", "Path tracing frames...")
        if renderer == "Cycles"
        else translate("Animation", "Generating frames..."),
        translate("Animation", "Cancel"),
        0,
        1,
        main,
    )
    dialog.setWindowModality(QtCore.Qt.WindowModal)
    dialog.setMinimumDuration(0)

    def progress(index, count):
        if index >= count:
            dialog.setLabelText(translate("Animation", "Encoding..."))
            dialog.setMaximum(0)
        else:
            dialog.setMaximum(count)
            dialog.setValue(index)
        QtWidgets.QApplication.processEvents()
        return not dialog.wasCanceled()

    try:
        done = exportAnimation(anim, path, view=view, progress=progress, renderer=renderer)
    except Cancelled:
        App.Console.PrintMessage("Animation export cancelled.\n")
        return None
    except Exception as e:  # pylint: disable=broad-except
        dialog.close()
        QtWidgets.QMessageBox.critical(main, title, str(e))
        return None
    finally:
        dialog.close()
    App.Console.PrintMessage(
        "Animation saved to %s: %d frames, %d x %d%s\n"
        % (
            done["Path"],
            done["Frames"],
            done["Size"][0],
            done["Size"][1],
            ", path traced" if done["Renderer"] == "Cycles" else "",
        )
    )
    return done
