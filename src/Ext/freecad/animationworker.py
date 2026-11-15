# SPDX-License-Identifier: LGPL-2.1-or-later
"""A worker process of an App::Animation (docs/Animation.md sec 4.2).

Run by the application in a ThunderealCmd of its own, not by hand: the
job file THUNDEREAL_ANIMATION_JOB names says which copy of the document to
open and where the frames go. Each line on the standard input is a frame's
number, made and written by Animation.writeFrame(); "quit" ends it. Every
answer is a line starting "FCAW " on the standard output, which nothing
else writes to: what else the process prints goes to its standard error:

    ready <resident>          the document is open
    done <frame> <resident>   the frame is in its file
    fail <frame> <why>        it could not be made
    unfit <why>               the document cannot be worked on here
"""

import io
import os
import sys
import traceback

import FreeCAD as App

JOB = "THUNDEREAL_ANIMATION_JOB"


_answers = 1


def _say(*words):
    text = "FCAW " + " ".join(str(w) for w in words)
    os.write(_answers, (text.replace("\n", " ") + "\n").encode("utf-8", "replace"))


def _quietOutput():
    """The answers on a descriptor of their own, and everything else this
    process prints -- a recompute's progress count, which leaves its line
    open -- in its log with its errors, so that nothing is printed into
    the middle of an answer"""
    global _answers
    try:
        sys.stdout.flush()
    except Exception:  # pylint: disable=broad-except
        pass
    _answers = os.dup(1)
    os.dup2(2, 1)


def _readJob(path):
    job = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if "=" in line:
                key, value = line.split("=", 1)
                job[key] = value
    return job


def _unfit(doc, proxies):
    """Why the document cannot be recomputed here, or None. An object with
    a Python class where the frames are wanted, `proxies`, whose class did
    not load here would recompute to nothing"""
    for name in proxies:
        obj = doc.getObject(name)
        if obj is None or getattr(obj, "Proxy", None) is None:
            return "%s has no Python class here" % (obj.Label if obj else name)
    return None


def serve():
    """The worker's loop; returns when told to quit or its input ends"""
    _quietOutput()
    try:
        job = _readJob(os.environ[JOB])
        doc = App.openDocument(job["Document"])
        anim = doc.getObject(job["Animation"])
        if anim is None or not anim.isDerivedFrom("App::Animation"):
            _say("unfit", "no animation %s in the copy" % job["Animation"])
            return
        why = _unfit(doc, job.get("Proxies", "").split())
        if why:
            _say("unfit", why)
            return
        # Nothing is kept here: each frame goes to its file and is let go
        doc.UndoMode = 0
        anim.Cache = False
        frames = job["Frames"]
        tables = job["Tables"]
    except Exception as e:  # pylint: disable=broad-except
        _say("unfit", "cannot open the document: %s" % e)
        return
    _say("ready", 0)
    stdin = io.open(0, "rb", closefd=False)
    while True:
        line = stdin.readline()
        if not line:
            break
        command = line.decode("utf-8", "replace").strip()
        if not command:
            continue
        if command == "quit":
            break
        try:
            index = int(command)
        except ValueError:
            continue
        try:
            written = anim.writeFrame(index, os.path.join(frames, "%d.wframe" % index), tables)
            _say("done", index, written["Resident"])
        except Exception as e:  # pylint: disable=broad-except
            App.Console.PrintLog(traceback.format_exc())
            _say("fail", index, e)
    try:
        App.closeDocument(doc.Name)
    except Exception:  # pylint: disable=broad-except
        pass
    sys.stdout.flush()
