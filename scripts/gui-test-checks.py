"""gui-test-checks.py <outdir> [exit code]: the checks of a GUI test run that hold
whatever the test does (docs/SmokeTesting.md sec 2.2, checks 1, 2 and 8).

Reads what scripts/gui-test.cmd and scripts/gui-test-guard.py left in <outdir>
when GT_CHECKS was on, and writes <outdir>\\checks.txt, one finding a line:

  log      an Err: or Wrn: line of FreeCAD.log (check 1; whether it is new is
           the suite's question, against a baseline -- scripts/gui-test-suite.py)
  dialog   a message box shown, with its text (check 2)
  end      the exit code, a crash log or dump in the test's home, a folder
           left in its transient folder, a document open at quit, a key of
           user.cfg the run changed (check 8). A run that started without a
           user.cfg (the test profile's) counts every key it wrote as added:
           that is the baseline the suite holds a clone run's keys against,
           since most keys a run changes are the test script's own.

Plain Python, no FreeCAD: run with any python 3.
"""
import os
import re
import sys
import xml.etree.ElementTree as ET


def log_lines(out):
    path = os.path.join(out, "FreeCAD.log")
    found = []
    if not os.path.exists(path):
        return found
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\r\n").lstrip(chr(0xFEFF))
            if line.startswith("Err:") or line.startswith("Wrn:"):
                found.append(line)
    return found


def normalised(line):
    """A log line without what changes from run to run: addresses, the run's own
    folders, times"""
    # the number FreeCAD puts after the prefix is a time, the same message
    # carries another in every run ("Err: 5.72002 <App> ...")
    line = re.sub(r"^(Err|Wrn|Msg|Log): [-+0-9.e]+ (?=<)", r"\1: <t> ", line)
    line = re.sub(r"0x[0-9a-fA-F]+", "0x?", line)
    line = re.sub(r"\b(FCT|tmp)[0-9A-Fa-f]+\.tmp\b", r"\1?.tmp", line)
    line = re.sub(r"[A-Za-z]:[\\/][^\s'\"]*?[\\/](home|temp)[\\/][^\s'\"]*", "<home>", line)
    line = re.sub(r"\b\d+(\.\d+)? ?(ms|s)\b", "<t>", line)
    return line


def cfg_values(path):
    """user.cfg as {group path/name: value}"""
    values = {}
    if not os.path.exists(path):
        return None
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as e:
        return {"<unreadable>": str(e)}

    def walk(node, prefix):
        for child in node:
            name = child.get("Name", "")
            if child.tag == "FCParamGroup":
                walk(child, prefix + "/" + name)
            elif child.tag in ("FCText",):
                values[prefix + "/" + name] = (child.text or "")
            elif child.tag.startswith("FC"):
                values[prefix + "/" + name] = child.get("Value", "")

    walk(root, "")
    return values


def end_findings(out, rc):
    found = []
    if rc not in (None, "0"):
        found.append("exit code %s" % rc)
    home = os.path.join(out, "home")
    for name in sorted(os.listdir(home)) if os.path.isdir(home) else []:
        low = name.lower()
        if low.startswith("crash") and (low.endswith(".log") or low.endswith(".dmp")):
            found.append("crash file %s" % name)
    temp = os.path.join(home, "temp")
    if os.path.isdir(temp):
        left = sorted(d for d in os.listdir(temp) if os.path.isdir(os.path.join(temp, d)))
        if left:
            found.append("transient folders left: %s" % left)
    path = os.path.join(out, "checks-end.txt")
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.endswith(": []"):
                    found.append(line)
    else:
        found.append("no record of the quit (the program did not reach aboutToQuit)")
    before = cfg_values(os.path.join(out, "user.cfg.start"))
    after = cfg_values(os.path.join(home, "user.cfg"))
    if before is None:
        before = {}  # no user.cfg at the start: every key is the run's
    if after is not None:
        for key in sorted(set(before) | set(after)):
            if before.get(key) != after.get(key):
                what = ("added" if key not in before else
                        "removed" if key not in after else "changed")
                found.append("user.cfg %s %s" % (what, key))
    return found


def main():
    out = sys.argv[1]
    rc = sys.argv[2] if len(sys.argv) > 2 else None
    lines = []
    for line in log_lines(out):
        lines.append("log | " + normalised(line))
    path = os.path.join(out, "checks-dialogs.txt")
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                if line.strip():
                    lines.append("dialog | " + line.strip())
    for f in end_findings(out, rc):
        lines.append("end | " + f)
    with open(os.path.join(out, "checks.txt"), "w", encoding="utf-8") as f:
        for line in lines:
            f.write(line + "\n")
    counts = {}
    for line in lines:
        kind = line.split(" | ", 1)[0]
        counts[kind] = counts.get(kind, 0) + 1
    print("gui-test checks: %s (%s)" % (
        ", ".join("%d %s" % (n, k) for k, n in sorted(counts.items())) or "nothing",
        os.path.join(out, "checks.txt")))


if __name__ == "__main__":
    main()
