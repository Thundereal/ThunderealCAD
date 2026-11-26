"""gui-test-suite.py: the tests/gui suite on Windows, in one mode or two, with the
checks of docs/SmokeTesting.md sec 2.2 (1 the log, 2 dialogs, 8 the end).

Each test registered in tests/gui/CMakeLists.txt (fc_add_gui_test) is run by
scripts/gui-test.cmd with GT_CHECKS=1, in each mode asked for:

  normal  the GUI test profile, as the suite always runs
  clone   on a COPY of a real profile (GT_CLONE): its user.cfg and system.cfg,
          no normalising, the window at its stored size, the screen's own DPI

and the two are compared test by test: a log line, a message box or an end
finding of the clone run that the normal run of the same test does not have
is a finding of the clone. summary.md and summary.json in --out say how many
pass in both, which fail only under the clone, and what the checks found.

A run already in --out with its verdict is not run again (--again to redo):
a suite of hours can be stopped and picked up.

  python scripts/gui-test-suite.py --out D:/runs/smoke --jobs 3
      --runner D:/path/runx.cmd --addons D:/path/test-addons/Mod
      [--modes normal,clone] [--clone %APPDATA%/Thundereal] [--filter regex]

--runner is FC_RUNNER for gui-test.cmd: a .cmd that puts the run-time on PATH
and runs the rest. The clone run gets --addons as its Mod; the normal run gets
none, as the suite always runs. The profile folder given to --clone is only
read, once: its user.cfg and system.cfg are copied to <out>/profile at the
start, and every clone run starts from that copy.
"""
import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def registered(cmake):
    """(name, script, timeout) of each fc_add_gui_test, in the file's order"""
    with open(cmake, encoding="utf-8") as f:
        text = f.read()
    text = re.sub(r"#[^\n]*", "", text)
    tests = []
    for m in re.finditer(r"fc_add_gui_test\(\s*(\S+)\s+(\S+)([^)]*)\)", text):
        name, script, rest = m.groups()
        if name == "name":  # the function's own definition
            continue
        t = re.search(r"--timeout\s+(\d+)", rest)
        tests.append((name, script, int(t.group(1)) if t else 300))
    return tests


def read(path):
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8", errors="replace") as f:
        return [line.rstrip("\r\n") for line in f]


def run_one(args, name, script, timeout, mode, desktop):
    out = os.path.join(args.out, mode, name)
    verdict_file = os.path.join(out, "suite-verdict.txt")
    if os.path.exists(verdict_file) and not args.again:
        return
    env = dict(os.environ)
    env.update({
        "GT_CHECKS": "1",
        "FC_PYTHON": sys.executable,
        "GT_DESKTOP_NAME": desktop,
        "FC_RUNNER": args.runner,
    })
    for key in ("GT_CLONE", "GT_ADDONS", "GT_SEED_CFG", "GT_EXPECT_OLD", "GT_NO_PROFILE",
                "GT_SCREEN"):
        env.pop(key, None)
    if args.build:
        env["FC_BUILD"] = args.build
    if mode == "clone":
        env["GT_CLONE"] = args.clone
        if args.addons:
            env["GT_ADDONS"] = args.addons
    os.makedirs(os.path.dirname(out), exist_ok=True)
    started = time.time()
    try:
        proc = subprocess.run(
            ["cmd", "/c", os.path.join(REPO, "scripts", "gui-test.cmd"),
             os.path.join(REPO, "tests", "gui", script), out, str(timeout)],
            env=env, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout + 240)
        rc = proc.returncode
        tail = proc.stdout.decode("utf-8", "replace")[-2000:]
    except subprocess.TimeoutExpired:
        rc, tail = "timeout", ""
    result = read(os.path.join(out, "result.txt"))
    fails = [l for l in result if l.startswith("FAIL") or l.startswith("ABORT")]
    verdict = {
        "name": name, "mode": mode, "rc": rc, "seconds": round(time.time() - started),
        "passed": rc == 0 and not fails and any(l.startswith("DONE") for l in result),
        "fails": fails[:5], "tail": tail if rc != 0 else "",
    }
    os.makedirs(out, exist_ok=True)
    with open(verdict_file, "w", encoding="utf-8") as f:
        json.dump(verdict, f)


def load(args, name, mode):
    out = os.path.join(args.out, mode, name)
    try:
        with open(os.path.join(out, "suite-verdict.txt"), encoding="utf-8") as f:
            verdict = json.load(f)
    except (OSError, ValueError):
        return None
    verdict["checks"] = read(os.path.join(out, "checks.txt"))
    return verdict


def summarise(args, tests, modes):
    rows = []
    cfg_keys = {}
    for name, script, _ in tests:
        v = {m: load(args, name, m) for m in modes}
        if any(x is None for x in v.values()):
            continue
        row = {"name": name, "script": script}
        for m in modes:
            row[m] = v[m]["passed"]
        if "clone" in modes and "normal" in modes:
            base = set(v["normal"]["checks"])
            new = [c for c in v["clone"]["checks"] if c not in base]
            row["new_in_clone"] = new
            row["clone_fails"] = v["clone"]["fails"]
            for c in new:
                if c.startswith("end | user.cfg"):
                    cfg_keys[c] = cfg_keys.get(c, 0) + 1
        for m in modes:
            row[m + "_checks"] = v[m]["checks"]
        rows.append(row)

    def count(pred):
        return [r for r in rows if pred(r)]

    lines = ["# GUI suite: %s" % ", ".join(modes), ""]
    lines.append("%d tests with a verdict in every mode, of %d registered." % (
        len(rows), len(tests)))
    if "clone" in modes and "normal" in modes:
        both = count(lambda r: r["normal"] and r["clone"])
        only_clone = count(lambda r: r["normal"] and not r["clone"])
        only_normal = count(lambda r: not r["normal"] and r["clone"])
        neither = count(lambda r: not r["normal"] and not r["clone"])
        lines += ["", "| | tests |", "|---|---|",
                  "| pass in both | %d |" % len(both),
                  "| fail only under the clone | %d |" % len(only_clone),
                  "| fail only on the test profile | %d |" % len(only_normal),
                  "| fail in both | %d |" % len(neither), ""]
        lines += ["## Fail only under the clone", ""]
        for r in only_clone:
            lines.append("- `%s`: %s" % (r["name"], "; ".join(r["clone_fails"][:3])))
        lines += ["", "## Fail in both", ""]
        for r in neither:
            lines.append("- `%s`" % r["name"])
        lines += ["", "## Checks: new under the clone (log, dialog, end; user.cfg keys below)",
                  ""]
        for r in rows:
            new = [c for c in r["new_in_clone"] if not c.startswith("end | user.cfg")]
            if new:
                lines.append("- `%s`" % r["name"])
                for c in new[:12]:
                    lines.append("  - %s" % c)
                if len(new) > 12:
                    lines.append("  - ... %d more" % (len(new) - 12))
        lines += ["", "## user.cfg keys the clone runs changed, by how many tests", ""]
        for key, n in sorted(cfg_keys.items(), key=lambda kv: -kv[1]):
            lines.append("- %d: %s" % (n, key[len("end | "):]))
    else:
        for m in modes:
            lines.append("%s: %d pass, %d fail" % (
                m, len(count(lambda r: r[m])), len(count(lambda r: not r[m]))))
    with open(os.path.join(args.out, "summary.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(args.out, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(rows, f, indent=1)
    print("\n".join(lines[:16]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--runner", required=True, help="FC_RUNNER for gui-test.cmd")
    ap.add_argument("--modes", default="normal,clone")
    ap.add_argument("--clone", default=os.path.join(os.environ.get("APPDATA", ""), "Thundereal"))
    ap.add_argument("--addons", default="")
    ap.add_argument("--build", default="")
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--filter", default="")
    ap.add_argument("--again", action="store_true")
    ap.add_argument("--summary-only", action="store_true")
    args = ap.parse_args()
    modes = [m for m in args.modes.split(",") if m]
    # patterns separated by "+" or ",": cmd takes a "|" for a pipe, and a
    # comma for the end of an argument
    patterns = [p for p in re.split(r"[+,]", args.filter) if p] or [""]
    tests = [t for t in registered(os.path.join(REPO, "tests", "gui", "CMakeLists.txt"))
             if any(re.search(p, t[0]) for p in patterns)
             and os.path.exists(os.path.join(REPO, "tests", "gui", t[1]))]
    os.makedirs(args.out, exist_ok=True)
    if "clone" in modes:
        # One copy taken at the start, and every clone run made from it: the
        # profile may be in use, and change, while a suite of hours runs
        import shutil

        snap = os.path.join(args.out, "profile")
        if not os.path.exists(os.path.join(snap, "user.cfg")):
            os.makedirs(snap, exist_ok=True)
            for name in ("user.cfg", "system.cfg"):
                src = os.path.join(args.clone, name)
                if os.path.exists(src):
                    shutil.copy2(src, os.path.join(snap, name))
        args.clone = snap
    if not args.summary_only:
        work = [(t, m) for t in tests for m in modes]
        desktops = ["fcad-gui-suite-%d" % i for i in range(args.jobs)]
        free = list(desktops)
        lock = threading.Lock()
        done = [0]

        def job(item):
            (name, script, timeout), mode = item
            with lock:
                desktop = free.pop()
            try:
                run_one(args, name, script, timeout, mode, desktop)
            finally:
                with lock:
                    free.append(desktop)
                    done[0] += 1
                    v = load(args, name, mode)
                    print("%4d/%d %-6s %-60s %s" % (
                        done[0], len(work), mode, name,
                        "PASS" if v and v["passed"] else "FAIL"), flush=True)

        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            list(pool.map(job, work))
    summarise(args, tests, modes)


if __name__ == "__main__":
    main()
