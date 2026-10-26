# Assembly workbench scaling benchmark (GUI run, startup macro).
#
# Run through a wrapper that defines CFG and execs this file, e.g.
#   CFG = dict(tag="s1", sizes=[10, 50], topos=["chain", "star"], drag=True,
#              profile_at=None, phase_limit=120.0, out=r"C:\some\scratch\dir")
#   _f = r"D:\works\sw\fcad\scripts\assembly-bench\bench.py"
#   exec(compile(open(_f).read(), _f, "exec"),
#        {"__name__": "__main__", "__file__": _f, "CFG": CFG})
# and launch the wrapper as a startup macro under the debugger:
#   run_cdb.ps1 -UserHome <empty dir> -StartupScript <wrapper.py>
#
# Results, profiles and temporary files go to CFG["out"] (or $ASM_BENCH_OUT),
# never into the source tree. The numbers in docs/AssemblyAudit.md sec 4 were
# taken with this script on 2026-10-22.
#
# Builds an assembly of N App::Link copies of one Part::Box, grounds the first,
# joins them with alternating Fixed / Revolute joints (chain: i-1 -> i,
# star: 0 -> i), and times each phase. Results go to results_<tag>.txt (human)
# and results_<tag>.jsonl (one record per measurement). A line "DONE" ends a run.
#
# CFG["joint_kind"]: "cpp" (the default) makes the joints as the joint task
# makes them since 2026-10-25, Assembly::JointPython with element links in
# the assembly's Joints group (docs/AssemblyObjectModel.md sec 7.1);
# "python" makes upstream's App::FeaturePython joints in a JointGroup, as the
# numbers of 2026-10-22 were taken.

import os
import sys
import time
import json
import traceback
import cProfile
import pstats
import io
import statistics

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore

HERE = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else "."
CFG = globals().get("CFG") or dict(tag="default", sizes=[10], topos=["chain"], drag=True)
PERF = CFG.get("out") or os.environ.get("ASM_BENCH_OUT")
if not PERF:
    raise RuntimeError("set CFG['out'] or ASM_BENCH_OUT to a scratch directory")
os.makedirs(PERF, exist_ok=True)
TAG = CFG.get("tag", "default")
OUT = os.path.join(PERF, "results_%s.txt" % TAG)
JSONL = os.path.join(PERF, "results_%s.jsonl" % TAG)
PHASE_LIMIT = float(CFG.get("phase_limit", 120.0))

pc = time.perf_counter


def log(msg):
    with open(OUT, "a") as f:
        f.write(msg + "\n")


def rec(**kw):
    with open(JSONL, "a") as f:
        f.write(json.dumps(kw) + "\n")


def pump(n=3):
    for _ in range(n):
        QtCore.QCoreApplication.processEvents()


def prefs():
    return App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")


# ---------------------------------------------------------------- native sampler

CONDA_PY = r"D:\works\sw\fcad\.conda\freecad\python.exe"


def start_sampler(name):
    import subprocess
    import threading

    out = os.path.join(PERF, "native_%s.txt" % name)
    stop = out + ".stop"
    for f in (out, stop, out + ".ready", out + ".done"):
        try:
            os.remove(f)
        except Exception:
            pass
    tid = threading.main_thread().native_id
    subprocess.Popen(
        [CONDA_PY, "-I", os.path.join(HERE, "sampler.py"), str(os.getpid()), str(tid), out, stop, "2"],
        creationflags=0x08000000,  # CREATE_NO_WINDOW
    )
    t0 = pc()
    while not os.path.exists(out + ".ready") and pc() - t0 < 120:
        time.sleep(0.2)
    log("  (native sampler started for %s after %.1fs)" % (name, pc() - t0))
    return out


def stop_sampler(out):
    open(out + ".stop", "w").write("stop")
    t0 = pc()
    while not os.path.exists(out + ".done") and pc() - t0 < 300:
        time.sleep(0.2)
    log("  (native sampler finished, %s)" % os.path.basename(out))


# ---------------------------------------------------------------- build


def placement_for(i):
    return App.Placement(
        App.Vector(12.0 * i + (i % 3), 1.5 * (i % 4), 0.7 * (i % 5)),
        App.Rotation(App.Vector(0, 0, 1), (7 * i) % 20),
    )


class Timer:
    def __init__(self):
        self.t0 = pc()

    def __call__(self):
        return pc() - self.t0


skip = {}  # (topo, phase) -> size at which it exceeded the limit


def should_skip(topo, phase, N):
    s = skip.get((topo, phase))
    return s is not None and N > s


def mark(topo, phase, N, dt):
    if dt > PHASE_LIMIT and (topo, phase) not in skip:
        skip[(topo, phase)] = N


def measure(topo, N, phase, fn, profile=False, extra=None):
    if phase in CFG.get("skip_phases", []) or phase in CFG.get("skip_by_topo", {}).get(topo, []):
        log("  %-28s skipped (config)" % phase)
        return None
    if should_skip(topo, phase, N):
        log("  %-28s skipped (exceeded %.0fs at smaller N)" % (phase, PHASE_LIMIT))
        rec(topo=topo, N=N, phase=phase, skipped=True)
        return None
    prof = cProfile.Profile() if profile else None
    sampler = None
    if (topo, N, phase) in [tuple(x) for x in CFG.get("native", [])]:
        sampler = start_sampler("%s_%s_%d_%s" % (TAG, topo, N, phase.split("(")[0]))
    t = Timer()
    if prof:
        prof.enable()
    try:
        r = fn()
    finally:
        if prof:
            prof.disable()
    dt = t()
    if sampler:
        stop_sampler(sampler)
    pump()
    log("  %-28s %10.4f s%s" % (phase, dt, ("  " + extra()) if extra else ""))
    rec(topo=topo, N=N, phase=phase, sec=dt)
    mark(topo, phase, N, dt)
    if prof:
        s = io.StringIO()
        st = pstats.Stats(prof, stream=s)
        st.sort_stats("cumulative").print_stats(45)
        st.sort_stats("tottime").print_stats(30)
        with open(os.path.join(PERF, "profile_%s_%s_%d_%s.txt" % (TAG, topo, N, phase)), "w") as f:
            f.write(s.getvalue())
    return r


def run_size(topo, N):
    import JointObject
    import UtilsAssembly

    log("== topo=%s N=%d" % (topo, N))
    prof_here = CFG.get("profile_at") == (topo, N)
    doc = App.newDocument("Bench_%s_%d" % (topo, N))
    App.setActiveDocument(doc.Name)
    box = doc.addObject("Part::Box", "Src")
    box.Length = 10
    box.Width = 10
    box.Height = 10
    doc.recompute()
    if box.ViewObject:
        box.ViewObject.Visibility = False
    asm = doc.addObject("Assembly::AssemblyObject", "Assembly")
    cpp = CFG.get("joint_kind", "cpp") == "cpp"
    if cpp:
        jg = asm.getGroup("Joints", True)
    else:
        jg = asm.newObject("Assembly::JointGroup", "Joints")
    pump()

    links = []

    def add_parts():
        for i in range(N):
            lk = asm.newObject("App::Link", CFG.get("name_fmt", "L%d") % i)
            lk.LinkedObject = box
            lk.Placement = placement_for(i)
            links.append(lk)

    measure(topo, N, "add_parts", add_parts)

    def ground():
        gj = jg.newObject(
            "Assembly::GroundedJointPython" if cpp else "App::FeaturePython", "GroundedJoint"
        )
        JointObject.GroundedJoint(gj, links[0])
        if gj.ViewObject:
            JointObject.ViewProviderGroundedJoint(gj.ViewObject)

    measure(topo, N, "ground", ground)

    joints = []
    per_joint = []

    def make_joint(i, other):
        j = jg.newObject("Assembly::JointPython" if cpp else "App::FeaturePython", "Joint")
        JointObject.Joint(j, 1 if (i % 2) else 0)  # Revolute / Fixed alternate
        if j.ViewObject:
            JointObject.ViewProviderJoint(j.ViewObject)
        refs = [[other, ["Face2", "Face2"]], [links[i], ["Face1", "Face1"]]]
        j.Proxy.setJointConnectors(j, refs)
        asm.clearUndo()
        return j

    def create_joints():
        prefs().SetBool("SolveInJointCreation", False)
        t0 = pc()
        for i in range(1, N):
            t = pc()
            joints.append(make_joint(i, links[i - 1] if topo == "chain" else links[0]))
            per_joint.append(pc() - t)
            if pc() - t0 > CFG.get("create_limit", 900.0):
                log("  create_joints aborted at %d joints (limit)" % len(joints))
                break
        prefs().SetBool("SolveInJointCreation", True)

    measure(
        topo,
        N,
        "create_joints_nosolve",
        create_joints,
        profile=prof_here,
        extra=lambda: "per-joint first10%%=%.2fms last10%%=%.2fms"
        % (
            1000 * statistics.mean(per_joint[: max(1, len(per_joint) // 10)]),
            1000 * statistics.mean(per_joint[-max(1, len(per_joint) // 10) :]),
        )
        if per_joint
        else "",
    )
    rec(topo=topo, N=N, phase="per_joint_ms", values=[round(1000 * x, 3) for x in per_joint])
    if len(joints) < N - 1:
        log("  incomplete build, stopping this size")
        App.closeDocument(doc.Name)
        return

    measure(topo, N, "recompute_first", lambda: doc.recompute(), profile=prof_here)
    measure(topo, N, "recompute_nochange", lambda: doc.recompute())

    def solve():
        r = asm.solve()
        if r != 0:
            log("  solve returned %r" % r)

    measure(topo, N, "solve_1", solve, profile=prof_here)
    try:
        gap = 0.0
        for j in joints:
            p1 = JointObject.sideJcsGlobal(j, 1)
            p2 = JointObject.sideJcsGlobal(j, 2)
            gap = max(gap, (p1.Base - p2.Base).Length)
        log("    max JCS origin gap after solve: %.3g mm" % gap)
        rec(topo=topo, N=N, phase="max_jcs_gap", value=gap)
    except Exception:
        log("    gap check failed: " + traceback.format_exc())
    if CFG.get("count_writes"):
        class Obs:
            def __init__(self):
                self.c = {}

            def slotChangedObject(self, obj, prop):
                k = "%s.%s" % (obj.TypeId, prop)
                self.c[k] = self.c.get(k, 0) + 1

        for label, fn in (("solve", solve), ("recompute_nochange", lambda: doc.recompute())):
            o = Obs()
            App.addDocumentObserver(o)
            try:
                fn()
            finally:
                App.removeDocumentObserver(o)
            top = sorted(o.c.items(), key=lambda kv: -kv[1])[:8]
            log("    property changes during %s: total %d  %s" % (label, sum(o.c.values()), top))
            rec(topo=topo, N=N, phase="writes_" + label, counts=o.c)
    measure(topo, N, "solve_2", solve)
    measure(topo, N, "solve_3", solve)

    measure(topo, N, "Joints_attr(getJoints)", lambda: asm.Joints)
    measure(topo, N, "isPartConnected(last)", lambda: asm.isPartConnected(links[-1]))
    measure(topo, N, "isPartGrounded(last)", lambda: asm.isPartGrounded(links[-1]))
    measure(
        topo, N, "getDownstreamParts(L1,J1)", lambda: asm.getDownstreamParts(links[1], joints[0])
    )
    asmt = os.path.join(PERF, "tmp_%s.asmt" % TAG)
    measure(topo, N, "exportAsASMT", lambda: asm.exportAsASMT(asmt))
    try:
        os.remove(asmt)
    except Exception:
        pass

    def redraw_all():
        for j in joints:
            j.Proxy.redrawJointPlacements(j)

    measure(topo, N, "py_redraw_all_joints", redraw_all, profile=prof_here)

    def getglobal_one():
        JointObject.sideJcsGlobal(joints[-1], 2)

    measure(topo, N, "py_sideJcsGlobal(1)", getglobal_one)

    def py_parents_one():
        return links[-1].Parents

    measure(topo, N, "py_Parents(1)", py_parents_one)

    # One small change: rotate the offset of the middle joint (triggers
    # Joint.onChanged -> updateJCSPlacements + preSolve/solve).
    mid = joints[len(joints) // 2]

    def change_offset():
        JointObject.setJointOffset(
            mid, 2, App.Placement(App.Vector(0, 0, 0), App.Rotation(App.Vector(1, 0, 0), 5))
        )

    measure(topo, N, "change_offset(onChanged)", change_offset, profile=prof_here)
    measure(topo, N, "recompute_after_change", lambda: doc.recompute(), profile=prof_here)

    # Touch one leaf part and recompute (what a part edit does).
    def touch_leaf():
        links[-1].touch()
        doc.recompute()

    measure(topo, N, "touch_leaf+recompute", touch_leaf)

    # Interactive add of one more part + joint (the joint task path).
    def add_one_interactive():
        lk = asm.newObject("App::Link", CFG.get("name_fmt", "L%d") % N)
        lk.LinkedObject = box
        lk.Placement = placement_for(N)
        links.append(lk)
        prefs().SetBool("SolveInJointCreation", True)
        t = pc()
        j = make_joint(len(links) - 1, links[-2] if topo == "chain" else links[0])
        t1 = pc() - t
        t = pc()
        asm.recompute(True)
        t2 = pc() - t
        joints.append(j)
        return t1, t2

    r = measure(topo, N, "add_one_joint_interactive", add_one_interactive, profile=prof_here)
    if r:
        log("    (setJointConnectors %.4fs, assembly.recompute(True) %.4fs)" % r)
        rec(topo=topo, N=N, phase="add_one_joint_split", setJointConnectors=r[0], recompute=r[1])

    if CFG.get("drag", True) and should_skip(topo, "preDrag", N):
        log("  drag skipped (preDrag exceeded the limit at a smaller N)")
    elif CFG.get("drag", True):
        try:
            run_drag(topo, N, doc, asm, links, joints, prof_here)
        except Exception:
            log("  drag failed: " + traceback.format_exc())

    pump()
    App.closeDocument(doc.Name)
    pump()


def run_drag(topo, N, doc, asm, links, joints, prof_here):
    # preDrag / doDragStep / postDrag are bound to Python since 2026-10-23;
    # before that this script called the exported C++ members through ctypes.

    # GUI drag init hides every joint but the moving one (tryInitMove).
    moving = joints[-1]
    part = links[-1]

    def hide_joints():
        for j in joints:
            if j is moving:
                continue
            if j.Visibility:
                j.Visibility = False
            j.purgeTouched()

    measure(topo, N, "drag_hide_joints", hide_joints)

    doc.openTransaction("Move part")
    measure(topo, N, "preDrag", lambda: asm.preDrag([part]), profile=prof_here)
    base = part.Placement
    steps = []
    redraws = []
    failed = 0
    view = None
    try:
        view = Gui.ActiveDocument.ActiveView
    except Exception:
        pass
    prof = cProfile.Profile() if prof_here else None
    nat = None
    if (topo, N, "doDragStep") in [tuple(x) for x in CFG.get("native", [])]:
        nat = start_sampler("%s_%s_%d_doDragStep" % (TAG, topo, N))
    for k in range(1, 11):
        part.Placement = base.multiply(
            App.Placement(App.Vector(0, 0, 0), App.Rotation(App.Vector(1, 0, 0), 3 * k))
        )
        t = pc()
        if prof:
            prof.enable()
        if not asm.doDragStep():
            failed += 1
        if prof:
            prof.disable()
        steps.append(pc() - t)
        t = pc()
        if view is not None:
            try:
                view.redraw()
            except Exception:
                pass
        pump(2)
        redraws.append(pc() - t)
    if nat:
        stop_sampler(nat)
    if prof:
        s = io.StringIO()
        st = pstats.Stats(prof, stream=s)
        st.sort_stats("cumulative").print_stats(40)
        with open(os.path.join(PERF, "profile_%s_%s_%d_doDragStep.txt" % (TAG, topo, N)), "w") as f:
            f.write(s.getvalue())
    med = statistics.median(steps)
    log(
        "  %-28s %10.4f s  (median of 10; min %.4f max %.4f)"
        % ("doDragStep", med, min(steps), max(steps))
    )
    log("  %-28s %10.4f s  (median of 10)" % ("redraw+pump per step", statistics.median(redraws)))
    if failed:
        log("    %d of 10 drag steps failed or did nothing" % failed)
    rec(topo=topo, N=N, phase="doDragStep", sec=med, all=steps)
    rec(topo=topo, N=N, phase="drag_redraw_pump", sec=statistics.median(redraws), all=redraws)
    measure(topo, N, "postDrag", lambda: asm.postDrag())
    measure(topo, N, "commitTransaction", lambda: doc.commitTransaction())

    def show_joints():
        for j in joints:
            if not j.Visibility:
                j.Visibility = True
            j.purgeTouched()

    measure(topo, N, "drag_restore_joints", show_joints)


def main():
    try:
        App.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
            "AutoSaveEnabled", False
        )
        log("# bench tag=%s cfg=%r pid=%d" % (TAG, CFG, os.getpid()))
        log("# python %s" % sys.version.split()[0])
        Gui.activateWorkbench("AssemblyWorkbench")
        pump()
        runs = CFG.get("runs") or [(t, n) for n in CFG["sizes"] for t in CFG["topos"]]
        prefs().SetBool("SolveOnRecompute", bool(CFG.get("solve_on_recompute", True)))
        for topo, N in runs:
            if True:
                try:
                    run_size(topo, N)
                except Exception:
                    log("  run_size failed: " + traceback.format_exc())
                    for name in list(App.listDocuments().keys()):
                        try:
                            App.closeDocument(name)
                        except Exception:
                            pass
    except Exception:
        log("FATAL " + traceback.format_exc())
    finally:
        for name in list(App.listDocuments().keys()):
            try:
                App.closeDocument(name)
            except Exception:
                pass
        log("DONE")
        QtCore.QTimer.singleShot(1000, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(3000, main)
