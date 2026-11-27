"""gui-test-attribute.py: which part of a real profile a test failing only under the clone
depends on (docs/SmokeTesting.md, step 1). Two ways, both over gui-test-suite.py --modes clone:

  steps   the tests failing only under the clone in <smoke>/summary.json, run again on
          variants of the profile snapshot <smoke>/profile/user.cfg, cumulative: each step
          takes one more group of settings back to the GUI test profile's state and runs only
          the tests still failing after the step before. A test that passes from a step on
          depends on that step's group; what still fails after the last is a real-defect
          candidate. Writes <out>/<step>/..., <out>/attribution.md and attribution.json.

  bisect  one test on one profile: halves of its leaf groups (the keys directly under one
          FCParamGroup) are taken away in turn until the one group is left whose removal makes
          the test pass. A step where neither half alone does is reported as two groups
          together, and the search stops there. Writes <out>/pNN/... and <out>/bisect.md.

  python scripts/gui-test-attribute.py steps --smoke D:/runs/smoke --out D:/runs/vary
      --runner D:/path/runx.cmd --addons D:/path/test-addons/Mod [--jobs 3]
  python scripts/gui-test-attribute.py bisect --cfg D:/runs/vary/aa/profile/user.cfg
      --test GuiAnimationTimeline_tests_run --out D:/runs/bisect --runner ... --addons ...

A step's group is read from the source tree where it can be (the Dark preference pack's
keys), so the steps follow the pack. The pack also carries the custom title bar and the
workbench tab bar; those are left to the window and perview steps, so a colour step does not
take the title bar away with the colours. Plain Python, no FreeCAD.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BA = "/Root/BaseApp"
PREF = BA + "/Preferences"
ANTIALIASING = PREF + "/View/AntiAliasing"
LAYOUT = {PREF + "/MainWindow/CustomTitleBar", PREF + "/MainWindow/TitleBarToolBars",
          PREF + "/Workbenches/ShowTabBar"}


def pack_keys(path):
    keys = set()

    def walk(n, p):
        for c in n:
            nm = c.get("Name", "")
            if c.tag == "FCParamGroup":
                walk(c, p + "/" + nm)
            elif c.tag.startswith("FC"):
                keys.add(p + "/" + nm)

    walk(ET.parse(path).getroot(), "")
    return keys


def steps():
    """(step, what it takes back, how); a key path ending in "/*" drops the whole group"""
    dark = pack_keys(os.path.join(REPO, "src", "Gui", "PreferencePacks", "Dark", "Dark.cfg"))
    mw = PREF + "/MainWindow/"
    return [
        ("aa", "View/AntiAliasing = 0, as the test profile sets it",
         {"set": {ANTIALIASING: ("FCInt", "0")}}),
        ("theme", "the theme's colours: the Dark pack's keys but the title bar and tab bar, "
                  "Theme*, the style sheets, Themes/*",
         {"drop": (dark - LAYOUT) | {mw + k for k in (
             "Theme", "ThemeAuto", "ThemeAutoApplied", "StyleSheet", "OverlayActiveStyleSheet",
             "MenuStyleSheet")} | {PREF + "/Themes/*"}}),
        ("docks", "the docks: MainWindow/DockWindows/*, View/DockOverlay*, "
                  "Preferences/DockWindows/* (combo view or tree and property views apart), "
                  "PropertyView/* (the tab last shown)",
         {"drop": {BA + "/MainWindow/DockWindows/*", PREF + "/DockWindows/*",
                   PREF + "/PropertyView/*"},
          "drop_prefix": [PREF + "/View/DockOverlay"]}),
        ("window", "the window: Maximized, Geometry, MainWindowState, the custom title bar, "
                   "toolbar places (MainWindow/ToolBars/*, MenuBarLeft/Right/*, StatusBar/*)",
         {"drop": {mw + k for k in ("Maximized", "Geometry", "MainWindowState")} | LAYOUT
          - {PREF + "/Workbenches/ShowTabBar"}
          | {BA + "/MainWindow/ToolBars/*", BA + "/MainWindow/MenuBarLeft/*",
             BA + "/MainWindow/MenuBarRight/*", BA + "/MainWindow/StatusBar/*",
             mw + "MenuBarLeft/*", mw + "MenuBarRight/*", mw + "Toolbars/*"}}),
        ("perview", "View/PerViewEdit and Workbenches/ShowTabBar",
         {"drop": {PREF + "/View/PerViewEdit", PREF + "/Workbenches/ShowTabBar"}}),
        ("sketcher", "Mod/Sketcher/*", {"drop": {PREF + "/Mod/Sketcher/*"}}),
        ("commands", "Preferences/Commands/* (the command-to-workbench record)",
         {"drop": {PREF + "/Commands/*"}}),
        ("addons", "no addons", {"addons": False}),
    ]


def apply(tree, how):
    root = tree.getroot()
    drop = set(how.get("drop", ()))
    prefixes = how.get("drop_prefix", [])

    def walk(n, p):
        for c in list(n):
            path = p + "/" + c.get("Name", "")
            if c.tag == "FCParamGroup":
                if path + "/*" in drop:
                    n.remove(c)
                    continue
                walk(c, path)
            elif path in drop or any(path.startswith(x) for x in prefixes):
                n.remove(c)

    walk(root, "")
    for path, (tag, value) in how.get("set", {}).items():
        parts = path.strip("/").split("/")
        node = root
        for g in parts[:-1]:  # from "Root", the group under FCParameters
            nxt = [c for c in node if c.tag == "FCParamGroup" and c.get("Name") == g]
            node = nxt[0] if nxt else ET.SubElement(node, "FCParamGroup", Name=g)
        for c in [c for c in node if c.get("Name") == parts[-1]]:
            node.remove(c)
        ET.SubElement(node, tag, Name=parts[-1], Value=value)


def verdicts(out, names):
    res = {}
    for n in names:
        try:
            with open(os.path.join(out, "clone", n, "suite-verdict.txt"), encoding="utf-8") as f:
                res[n] = json.load(f)["passed"]
        except (OSError, ValueError):
            res[n] = None
    return res


def write_profile(tree, src_dir, out):
    prof = os.path.join(out, "profile")
    os.makedirs(prof, exist_ok=True)
    tree.write(os.path.join(prof, "user.cfg"), encoding="utf-8", xml_declaration=True)
    sysc = os.path.join(src_dir, "system.cfg")
    if os.path.exists(sysc):
        shutil.copy2(sysc, os.path.join(prof, "system.cfg"))
    return prof


def suite(a, out, prof, names, addons=True):
    args = [sys.executable, os.path.join(REPO, "scripts", "gui-test-suite.py"), "--out", out,
            "--runner", a.runner, "--modes", "clone", "--clone", prof, "--jobs", str(a.jobs),
            "--filter", "+".join("^%s$" % n for n in names)]
    if addons and a.addons:
        args += ["--addons", a.addons]
    with open(os.path.join(out, "suite.log"), "w", encoding="utf-8") as log:
        subprocess.run(args, stdout=log, stderr=subprocess.STDOUT, cwd=REPO)
    return verdicts(out, names)


def run_steps(a):
    with open(os.path.join(a.smoke, "summary.json"), encoding="utf-8") as f:
        rows = json.load(f)
    failing = [r["name"] for r in rows if r["normal"] and not r["clone"]]
    os.makedirs(a.out, exist_ok=True)
    src = os.path.join(a.smoke, "profile")
    tree = ET.parse(os.path.join(src, "user.cfg"))
    addons = True
    lines = ["# Clone-only failures: which part of the profile", "",
             "%d tests, steps cumulative." % len(failing), ""]
    fixed_by = {}
    for step, what, how in steps():
        apply(tree, how)
        if how.get("addons") is False:
            addons = False
        out = os.path.join(a.out, step)
        prof = write_profile(tree, src, out)
        v = suite(a, out, prof, failing, addons) if failing else {}
        passed = [n for n in failing if v[n]]
        for n in passed:
            fixed_by[n] = step
        lines += ["## %s: %s" % (step, what), "", "%d of %d now pass:" % (len(passed), len(failing))]
        lines += ["- `%s`" % n for n in passed] + [""]
        failing = [n for n in failing if not v[n]]
        with open(os.path.join(a.out, "attribution.md"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines + ["## still failing after this step", ""]
                              + ["- `%s`" % n for n in failing]) + "\n")
    with open(os.path.join(a.out, "attribution.json"), "w", encoding="utf-8") as f:
        json.dump({"fixed_by": fixed_by, "still_failing": failing}, f, indent=1)
    print("ALL-DONE", len(fixed_by), "attributed,", len(failing), "still failing")


def leaf_groups(root):
    found = []

    def walk(n, p):
        if any(c.tag != "FCParamGroup" for c in n):
            found.append(p)
        for c in n:
            if c.tag == "FCParamGroup":
                walk(c, p + "/" + c.get("Name", ""))

    walk(root, "")
    return [g for g in found if g]


def without(src, drop):
    """src's tree without the keys directly under the groups in drop (AntiAliasing kept)"""
    tree = ET.parse(src)

    def walk(n, p):
        for c in list(n):
            path = p + "/" + c.get("Name", "")
            if c.tag == "FCParamGroup":
                walk(c, path)
            elif p in drop and path != ANTIALIASING:
                n.remove(c)

    walk(tree.getroot(), "")
    return tree


def run_bisect(a):
    os.makedirs(a.out, exist_ok=True)
    cand = leaf_groups(ET.parse(a.cfg).getroot())
    log = open(os.path.join(a.out, "bisect.md"), "w", encoding="utf-8")

    def say(s):
        log.write(s + "\n")
        log.flush()
        print(s, flush=True)

    say("# %s: %d groups in %s" % (a.test, len(cand), a.cfg))
    n = [0]

    def passes(drop):
        n[0] += 1
        out = os.path.join(a.out, "p%02d" % n[0])
        prof = write_profile(without(a.cfg, set(drop)), os.path.dirname(a.cfg), out)
        ok = bool(suite(a, out, prof, [a.test])[a.test])
        say("- p%02d: without %d groups: %s" % (n[0], len(drop), "PASS" if ok else "fail"))
        return ok

    if not passes(cand):
        say("still fails with every group gone: not the profile's settings")
        return
    while len(cand) > 1:
        half = len(cand) // 2
        if passes(cand[:half]):
            cand = cand[:half]
        elif passes(cand[half:]):
            cand = cand[half:]
        else:
            say("neither half alone: groups together, among %s" % cand)
            break
    say("## needs: %s" % cand)
    with open(os.path.join(a.out, "bisect.json"), "w", encoding="utf-8") as f:
        json.dump(cand, f, indent=1)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="how", required=True)
    for name in ("steps", "bisect"):
        p = sub.add_parser(name)
        p.add_argument("--out", required=True)
        p.add_argument("--runner", required=True, help="FC_RUNNER for gui-test.cmd")
        p.add_argument("--addons", default="")
        p.add_argument("--jobs", type=int, default=3 if name == "steps" else 1)
        if name == "steps":
            p.add_argument("--smoke", required=True, help="a gui-test-suite.py --out")
        else:
            p.add_argument("--cfg", required=True, help="the user.cfg to bisect")
            p.add_argument("--test", required=True)
    a = ap.parse_args()
    run_steps(a) if a.how == "steps" else run_bisect(a)


if __name__ == "__main__":
    main()
