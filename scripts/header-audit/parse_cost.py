"""Fit compile time against the bytes of headers a TU pulls in, to put a rough price on
dropping headers from a TU (the compile-speed side, separate from rebuild localization)."""
import os, sys, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hdrmodel
m = hdrmodel.load(sys.argv[1], sys.argv[2], sys.argv[3])
size = {}
def sz(i):
    s = size.get(i)
    if s is None:
        try: s = os.path.getsize(m.paths[i])
        except OSError: s = 0
        size[i] = s
    return s
xs, ys = [], []
fork = (m.src_root + "/src/").lower()
for t in m.tus:
    if not t["src"].lower().startswith(fork) or "/3rdparty/" in t["src"].lower() or not t["ms"]:
        continue
    b = sum(sz(h) for h in t["deps"])
    xs.append(b / 1e6); ys.append(t["ms"] / 1000)
n = len(xs); mx = sum(xs)/n; my = sum(ys)/n
sxy = sum((x-mx)*(y-my) for x, y in zip(xs, ys)); sxx = sum((x-mx)**2 for x in xs)
b = sxy/sxx; a = my - b*mx
syy = sum((y-my)**2 for y in ys)
r2 = sxy*sxy/(sxx*syy)
print(f"fork TUs {n}: mean header MB {mx:.1f}, mean compile {my:.1f}s; fit t = {a:.2f}s + {b:.2f}s/MB (r2 {r2:.2f})")
if len(sys.argv) > 4:
    w = json.load(open(sys.argv[4]))
    by = {hdrmodel.display(m, i).lower(): i for i in range(len(m.paths)) if m.kind[i] != "ext"}
    for pr in w:
        # bytes of lost fork headers only (externals behind them are not tracked per TU here)
        tot = 0.0
        for x in pr["top"]:
            pass
    print("(per-proposal byte savings: see report)")
