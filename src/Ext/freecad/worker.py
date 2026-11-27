# SPDX-License-Identifier: LGPL-2.1-or-later
"""Worker processes (docs/WorkerProcess.md).

The pool of a document, App.workerPool(doc), runs jobs in console
processes of their own, each holding a copy of the document. A job has a
kind; a kind is done by a handler registered where the job runs, in C++
(App::WorkerPool::registerKind) or here:

    from freecad import worker

    def thicken(doc, inputs, out):
        ...              # doc: the worker's copy; out: a file it may write
        return {"Volume": shape.Volume}    # JSON, the job's output

    worker.register("mymod.thicken", thicken)

and submitted with the module that registers it, which a worker imports
before its first job of the kind:

    pool = App.workerPool(doc)
    pool.start()
    job = pool.submit("mymod.thicken", {"Object": "Pad"}, module="mymod")
    pool.wait()
    pool.result(job)["Output"]

A handler raises Unfit for a job it cannot do faithfully there; the client
then does it itself. serve() is the worker's loop, run by the pool, not by
hand.
"""

import importlib
import json
import sys
import traceback

import FreeCAD as App


class Unfit(Exception):
    """Raised by a handler for a job its worker cannot do faithfully"""


_kinds = {}


def register(kind, handler):
    """handler(doc, inputs, out) -> dict or None, for jobs of `kind`"""
    _kinds[kind] = handler


def kinds():
    return sorted(_kinds)


def _reply(**what):
    return json.dumps(what)


def _run(kind, docName, inputs, out, module):
    """Called by the worker's loop for a kind not in C++: what came of it,
    as JSON"""
    try:
        if kind not in _kinds and module:
            importlib.import_module(module)
        handler = _kinds.get(kind)
        if handler is None:
            return _reply(ok=False, unfit=True, error="no handler for %s here" % kind)
        doc = App.getDocument(docName) if docName else None
        from freecad import worker_gui

        before = worker_gui.faked()
        output = handler(doc, json.loads(inputs), out)
        if worker_gui.faked() != before:
            # It used what the worker only stands in for -- an exporter
            # reading a view object's colour -- and its result is not the
            # one the GUI would make (docs/WorkerProcess.md sec 72)
            return _reply(ok=False, unfit=True,
                          error="%s used FreeCADGui or a view object, which a worker "
                                "process has not got" % kind)
        return _reply(ok=True, output=output if output is not None else {})
    except Unfit as e:
        return _reply(ok=False, unfit=True, error=str(e) or kind)
    except Exception as e:  # pylint: disable=broad-except
        App.Console.PrintLog(traceback.format_exc())
        return _reply(ok=False, error=str(e) or type(e).__name__)


def _export(doc, inputs, out):
    """Kind "export" (docs/WorkerProcess.md sec 24): Module.export(objs, file
    [, options]) on the worker's copy, the call an export here makes.

    inputs: module, objects (names, in order), file, options (a dict, or
    None for a module that takes none), keywords (a dict of keyword
    arguments, or none), settings (a file of this process's Preferences as
    they were when it was asked for, or empty)"""
    del out
    if doc is None:
        raise Unfit("no document")
    objs = []
    for name in inputs.get("objects", []):
        obj = doc.getObject(name)
        if obj is None:
            raise Unfit("no object %s in the copy" % name)
        objs.append(obj)
    settings = inputs.get("settings")
    if settings:
        # What an exporter reads of the preferences, as the client has them
        # now: the worker's own copy is from when it was started
        App.ParamGet("User parameter:BaseApp/Preferences").Import(settings)
        # The STEP and IGES writers' own, which Part took from them once
        try:
            import Part

            Part.applyExchangeSettings()
        except ImportError:
            pass
    module = importlib.import_module(inputs["module"])
    options = inputs.get("options")
    keywords = inputs.get("keywords") or {}
    if options is None:
        module.export(objs, inputs["file"], **keywords)
    else:
        module.export(objs, inputs["file"], options, **keywords)
    return {"file": inputs["file"]}


register("export", _export)


def _check(doc, inputs, out):
    """Kind "part.check" (docs/WorkerProcess.md sec 25): Part_CheckGeometry's
    checks of each item -- an object and a subname, the shape the panel
    checks -- with the panel's options, its findings by position
    (Part.checkGeometry)"""
    del out
    import Part

    if doc is None:
        raise Unfit("no document")
    results = []
    for item in inputs.get("items", []):
        obj = doc.getObject(item["object"])
        if obj is None:
            raise Unfit("no object %s in the copy" % item["object"])
        shape = Part.getShape(obj, item.get("sub", ""), needSubElement=True)
        found = [] if shape.isNull() else Part.checkGeometry(shape, inputs.get("options"))
        results.append({"object": item["object"], "sub": item.get("sub", ""),
                        "findings": [list(f) for f in found]})
    return {"results": results}


register("part.check", _check)


def serve():
    """The worker's loop; returns when the pool says quit or goes"""
    try:
        sys.stdout.flush()
    except Exception:  # pylint: disable=broad-except
        pass
    # Before the copy opens: an addon's module that calls FreeCADGui at
    # import loads (docs/WorkerProcess.md sec 72)
    from freecad import worker_gui

    worker_gui.install()
    return App._serveWorker()
