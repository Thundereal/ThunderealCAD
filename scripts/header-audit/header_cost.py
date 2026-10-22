"""Rank fork-owned headers by the rebuild cost of touching them.

  python header_cost.py <build_dir> <src_root> <deps.txt> [--top N] [--csv out.csv]
                        [--churn churn.tsv] [--jobs 6] [--show PATTERN ...]

Get deps.txt with:  ninja -C <build_dir> -t deps > deps.txt
(on the Windows box: cmd //c 'D:\\works\\sw\\fcad\\.conda\\run.cmd ninja -C <build> -t deps')

cost(H) = sum of compile ms of every TU whose deps contain H
        + sum of link ms of every link step those TUs feed (static libs propagate one
          level; DLL import libs are restat=1 and only relink consumers on an export change,
          so they are not counted).
Wall estimate = compile CPU / jobs + link ms (links of one change are few and mostly
serialized at the end).
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hdrmodel  # noqa: E402


def compute(m):
    tus_of = collections.defaultdict(list)
    for ti, tu in enumerate(m.tus):
        for h in tu["deps"]:
            if m.kind[h] != "ext":
                tus_of[h].append(ti)
    return tus_of


def cost_of(m, tis):
    cms = sum((m.tus[t]["ms"] or 0) for t in tis)
    links = set()
    for t in tis:
        links.update(m.tus[t]["links"])
    lms = sum(m.link_ms.get(l, 0) for l in links)
    return cms, lms, len(links)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build")
    ap.add_argument("src")
    ap.add_argument("deps")
    ap.add_argument("--top", type=int, default=60)
    ap.add_argument("--csv")
    ap.add_argument("--churn", help="tsv: count<TAB>path (relative to src root)")
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--show", nargs="*", default=[])
    ap.add_argument("--baseline", help="a ranking.csv from an earlier run: report headers whose "
                    "TU count grew by more than --grow TUs")
    ap.add_argument("--grow", type=int, default=25)
    a = ap.parse_args()

    m = hdrmodel.load(a.build, a.src, a.deps)
    tus_of = compute(m)
    total_ms = sum((t["ms"] or 0) for t in m.tus)
    missing = sum(1 for t in m.tus if t["ms"] is None)
    all_links = set()
    for t in m.tus:
        all_links.update(t["links"])
    total_link = sum(m.link_ms.get(l, 0) for l in all_links)
    print(f"TUs: {len(m.tus)} (no log entry: {missing})")
    print(f"compile CPU: {total_ms/1000/60:.1f} min  (~{total_ms/1000/60/a.jobs:.1f} min wall at -j{a.jobs})")
    print(f"link steps fed by TUs: {len(all_links)}, {total_link/1000/60:.1f} min")
    fork_tus = [t for t in m.tus if t["src"].lower().startswith((m.src_root + "/src/").lower())
                and "/3rdparty/" not in t["src"].lower()]
    print(f"fork TUs (src/ minus 3rdParty): {len(fork_tus)}, "
          f"{sum((t['ms'] or 0) for t in fork_tus)/60000:.1f} min CPU")

    churn = {}
    if a.churn:
        for line in open(a.churn, encoding="utf-8"):
            c, p = line.rstrip("\n").split("\t", 1)
            churn[p.lower()] = int(c)

    oth = collections.defaultdict(list)
    for o in getattr(m, "others", []):
        for h in o["deps"]:
            if m.kind[h] != "ext":
                oth[h].append(o["ms"])
    rows = []
    for h, tis in tus_of.items():
        cms, lms, nl = cost_of(m, tis)
        d = hdrmodel.display(m, h)
        rows.append(dict(h=h, path=d, tus=len(tis), cms=cms, lms=lms, nlinks=nl,
                         nauto=len(oth[h]), ams=sum(oth[h]),
                         wall=cms / a.jobs + lms + sum(oth[h]) / a.jobs,
                         churn=churn.get(d.lower(), 0)))
    rows.sort(key=lambda r: -(r["cms"] + r["lms"]))
    if a.csv:
        with open(a.csv, "w", encoding="utf-8") as f:
            f.write("rank,path,tus,compile_cpu_min,links,link_min,autogen,steps,wall_min_j%d,churn\n" % a.jobs)
            for i, r in enumerate(rows, 1):
                f.write(f"{i},{r['path']},{r['tus']},{r['cms']/60000:.2f},{r['nlinks']},"
                        f"{r['lms']/60000:.2f},{r['nauto']},{r['tus']+r['nlinks']+r['nauto']},"
                        f"{r['wall']/60000:.2f},{r['churn']}\n")
    print()
    print(f"{'#':>3} {'TUs':>5} {'CPUmin':>7} {'lnk':>4} {'wall':>6} {'churn':>5}  header")
    for i, r in enumerate(rows[: a.top], 1):
        print(f"{i:>3} {r['tus']:>5} {r['cms']/60000:>7.1f} {r['nlinks']:>4} "
              f"{r['wall']/60000:>6.1f} {r['churn']:>5}  {r['path']}")
    for pat in a.show:
        for i, r in enumerate(rows, 1):
            if r["path"].lower().replace("\\", "/").endswith(pat.lower()):
                print(f"SHOW rank {i}: {r['path']} TUs={r['tus']} CPU={r['cms']/60000:.1f}min "
                      f"links={r['nlinks']} ({r['lms']/60000:.1f}min) autogen={r['nauto']} "
                      f"steps={r['tus']+r['nlinks']+r['nauto']} "
                      f"wall~{r['wall']/60000:.1f}min")
    if a.baseline:
        base = {}
        import csv
        for r in csv.DictReader(open(a.baseline, encoding="utf-8")):
            base[r["path"].lower()] = int(r["tus"])
        grew = [(r["tus"] - base.get(r["path"].lower(), 0), r) for r in rows
                if r["tus"] - base.get(r["path"].lower(), 0) > a.grow]
        grew.sort(key=lambda x: -x[0])
        print(f"\nheaders whose reach grew by more than {a.grow} TUs since the baseline: {len(grew)}")
        for d, r in grew[:40]:
            print(f"  +{d:>5} TUs -> {r['tus']:>5}  {r['path']}")
    if churn:
        tot = sum(r["churn"] * (r["cms"] + r["lms"]) for r in rows)
        print(f"\nchurn-weighted rebuild CPU (sum edits*cost): {tot/3600000:.0f} CPU-hours")
        rows.sort(key=lambda r: -r["churn"] * (r["cms"] + r["lms"]))
        print("top by churn*cost:")
        for r in rows[:30]:
            print(f"  {r['churn']:>4} edits x {(r['cms']+r['lms'])/60000:>6.1f} min = "
                  f"{r['churn']*(r['cms']+r['lms'])/3600000:>6.1f} CPU-h  {r['path']}")


if __name__ == "__main__":
    main()
