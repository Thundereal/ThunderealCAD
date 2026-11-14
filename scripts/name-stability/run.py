# Run by FreeCADCmd (see run.sh): every case and change of cases.py, or those
# whose name holds NAMES_ONLY, counted into NAMES_OUT (.json and .md).
import collections
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cases  # noqa: E402
import harness  # noqa: E402

out = os.environ.get("NAMES_OUT", "/tmp/name-stability")
only = os.environ.get("NAMES_ONLY", "")
O = harness.OUTCOMES
results = []
for fn in cases.CASES:
    if only and only not in fn.__name__:
        continue
    for label, change in fn.changes:
        r = harness.run(fn, label, change)
        results.append(r)
        with open(out + ".progress", "a") as f:
            f.write(
                "%s / %s: %s\n"
                % (
                    r["case"],
                    label,
                    dict((o, sum(n for k, n in r["counts"].items() if k[4] == o)) for o in O),
                )
            )


def table(head, rows):
    lines = ["| " + " | ".join(head) + " |", "| " + " | ".join(["---"] * len(head)) + " |"]
    for row in rows:
        lines.append("| " + " | ".join(str(c) for c in row) + " |")
    return "\n".join(lines)


def tally(counter, key):
    t = collections.defaultdict(collections.Counter)
    for k, n in counter.items():
        t[key(k)][k[4]] += n
    return t


def rows_of(t, order=None):
    keys = order or sorted(t, key=lambda k: -sum(t[k].values()))
    return [[k] + [t[k][o] or "" for o in O] + [sum(t[k].values())] for k in keys if k in t]


md = []
md.append("## By case and change\n")
rows = []
for r in results:
    c = collections.Counter()
    for k, n in r["counts"].items():
        c[k[4]] += n
    p = collections.Counter()
    for k, n in r["pure"].items():
        p[k[4]] += n
    rows.append(
        [r["case"], r["change"]]
        + [c[o] or "" for o in O]
        + [sum(c.values())]
        + [p["lost"] or "", p["WRONG"] or ""]
    )
md.append(table(["case", "change"] + list(O) + ["all", "name lost", "name WRONG"], rows))

total, pure = collections.Counter(), collections.Counter()
for r in results:
    total.update(r["counts"])
    pure.update(r["pure"])
for title, counter in (
    ("the references", total),
    ("the names alone, nothing looking for a missing one", pure),
):
    md.append("\n## %s: by the nearest step holding a position\n" % title.capitalize())
    md.append(table(["via"] + list(O) + ["all"], rows_of(tally(counter, lambda k: k[2]))))
    md.append("\n## %s: by the name's own step\n" % title.capitalize())
    md.append(table(["own"] + list(O) + ["all"], rows_of(tally(counter, lambda k: k[1]))))
    md.append("\n## %s: by element type\n" % title.capitalize())
    md.append(
        table(
            ["type"] + list(O) + ["all"],
            rows_of(tally(counter, lambda k: k[0]), ["Face", "Edge", "Vertex"]),
        )
    )
    md.append("\n## %s: by root, a named input or one with no names\n" % title.capitalize())
    md.append(
        table(
            ["root"] + list(O) + ["all"],
            rows_of(tally(counter, lambda k: "no names" if k[3] else "named")),
        )
    )
md.append("\n## Notes and examples\n")
for r in results:
    for n in r["notes"]:
        md.append("- %s / %s: NOTE %s" % (r["case"], r["change"], n.strip().replace("\n", "\n  ")))
    for e in r["features"]:
        md.append("- %s / %s: %s" % (r["case"], r["change"], e))
    for e in r["examples"]:
        md.append("- %s / %s: %s" % (r["case"], r["change"], e))
open(out + ".md", "w").write("\n".join(md) + "\n")
json.dump(
    [
        dict(
            case=r["case"],
            change=r["change"],
            notes=r["notes"],
            examples=r["examples"],
            features=r["features"],
            counts=[list(k) + [n] for k, n in r["counts"].items()],
            pure=[list(k) + [n] for k, n in r["pure"].items()],
        )
        for r in results
    ],
    open(out + ".json", "w"),
    indent=1,
)
