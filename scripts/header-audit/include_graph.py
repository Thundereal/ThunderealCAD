"""Per-TU include graphs reconstructed from #include lines, restricted to each TU's real
header set (from .ninja_deps), so conditional/stale includes do not invent edges.

Subcommands (all take: <build_dir> <src_root> <deps.txt>):
  analyze  --targets FILE   per target header: direct parents, dominators ("umbrellas" every
                            path goes through), whether the .cpp includes it directly; plus
                            external-family (Qt/Coin/OCCT/Python/...) gateways.  -> JSON
  whatif   --spec FILE      remove include edges (parent -> child), optionally keeping the child
                            in TUs whose own .cpp/.h text matches a "needs" regex (they would add
                            the include themselves), and report which TUs every fork header
                            stops reaching and the rebuild CPU that saves.          -> JSON

Spec file format for whatif (one proposal per block, blank line separates):
  name: Some proposal
  cut: src/Gui/Application.h -> src/Gui/Document.h
  cut: src/App/Property.h -> src/App/FileBlobManager.h
  needs: \bFileBlobManager\b|\bBlobRef\b
"""
import argparse
import collections
import json
import multiprocessing as mp
import os
import pickle
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hdrmodel  # noqa: E402

INC = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')
COND = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$")
TRUE = {"FC_OS_WIN32", "_MSC_VER", "_WIN32", "WIN32", "_WIN64", "FC_OS_WIN64"}
FALSE = {"_PreComp_", "FC_OS_LINUX", "FC_OS_MACOSX", "FC_OS_BSD", "__APPLE__", "__linux__",
         "__EMSCRIPTEN__", "FC_OS_CYGWIN", "__MINGW32__"}

FAMILIES = [
    ("occt", "/occt/install/"),
    ("coin", "/include/inventor/"),
    ("qt", "/include/qt6/"),
    ("pyside", "/include/pyside6"),
    ("pyside", "/include/shiboken6"),
    ("python", "/.conda/freecad/include/"),
    ("boost", "/library/include/boost/"),
    ("vtk", "/include/vtk-"),
    ("smesh", "/include/smesh"),
    ("eigen", "/include/eigen3/"),
    ("xerces", "/include/xercesc/"),
    ("bgfx", "/3rdparty/bgfx/"),
]


def family(norm):
    for f, pat in FAMILIES:
        if pat in norm:
            return f
    return None


def _eval_cond(kind, expr):
    expr = expr.split("//")[0].split("/*")[0].strip()
    if kind == "ifdef":
        return False if expr in FALSE else True if expr in TRUE else None
    if kind == "ifndef":
        return True if expr in FALSE else False if expr in TRUE else None
    e = expr
    unknown = [False]

    def rep(mo):
        n = mo.group(1)
        if n in FALSE:
            return " False "
        if n in TRUE:
            return " True "
        unknown[0] = True
        return " True "

    e = re.sub(r"defined\s*\(\s*(\w+)\s*\)", rep, e)
    e = re.sub(r"defined\s+(\w+)", rep, e)
    if unknown[0]:
        return None
    e = e.replace("&&", " and ").replace("||", " or ").replace("!", " not ")
    if re.fullmatch(r"[\sA-Za-z()0-9]*", e) and not re.search(r"[A-Za-z_]\w*", re.sub(r"\b(and|or|not|True|False)\b", "", e)):
        try:
            return bool(eval(e, {}, {}))
        except Exception:
            return None
    return None


def parse_includes(path):
    """Return list of (angle, spec) active under the known-macro approximation."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
    except OSError:
        return []
    out = []
    stack = []  # entries: [active, taken, parent_active]  (taken: a known-true branch seen)
    active = True
    for line in lines:
        mo = COND.match(line)
        if mo:
            k, rest = mo.group(1), mo.group(2).strip()
            if k in ("if", "ifdef", "ifndef"):
                v = _eval_cond(k, rest)
                stack.append([active, v is True, v])
                active = active and (v is not False)
            elif k == "elif":
                if not stack:
                    continue
                par, taken, _ = stack[-1]
                v = _eval_cond("if", rest)
                if taken:
                    active = False
                else:
                    active = par and (v is not False)
                    if v is True:
                        stack[-1][1] = True
            elif k == "else":
                if not stack:
                    continue
                par, taken, v0 = stack[-1]
                active = par and not taken and not (v0 is True)
                if v0 is None and not taken:
                    active = par
            elif k == "endif":
                if stack:
                    active = stack.pop()[0]
            continue
        if not active:
            continue
        mi = INC.match(line)
        if mi:
            out.append((mi.group(1) == "<", mi.group(2).strip()))
    return out


class Graph:
    """Global include data: per file (header id, or ('src', tu index)) -> candidate lists."""


def build_graph(m, cache):
    if os.path.exists(cache) and os.path.getmtime(cache) > os.path.getmtime(m.build_dir + "/.ninja_deps"):
        with open(cache, "rb") as f:
            return pickle.load(f)
    by_base = collections.defaultdict(list)
    for i, n in enumerate(m.norm):
        by_base[n.rsplit("/", 1)[-1]].append(i)

    def cands(fromdir, spec):
        s = spec.replace("\\", "/").lower()
        if s.startswith("./"):
            s = s[2:]
        exact = os.path.normpath(fromdir + "/" + s).replace("\\", "/").lower()
        out = []
        if exact in m.ids:
            return [m.ids[exact]]
        if ".." not in s:
            base = s.rsplit("/", 1)[-1]
            for i in by_base.get(base, ()):
                if (m.norm[i].endswith("/" + s)) and i not in out:
                    out.append(i)
        return out

    g = Graph()
    g.hdr = {}  # header id -> [cand list,...]
    for i, n in enumerate(m.norm):
        # external headers are parsed too, so dropping an include also drops the boost/Qt/
        # OCCT headers behind it (their closure resolves inside each TU's real header set)
        d = os.path.dirname(m.paths[i])
        g.hdr[i] = [c for c in (cands(d, s) for a, s in parse_includes(m.paths[i])) if c]
    g.src = []
    for tu in m.tus:
        d = os.path.dirname(tu["src"])
        g.src.append([c for c in (cands(d, s) for a, s in parse_includes(tu["src"])) if c])
    g.fam = [family(n) for n in m.norm]
    with open(cache, "wb") as f:
        pickle.dump(g, f, protocol=pickle.HIGHEST_PROTOCOL)
    return g


# ---------------------------------------------------------------- worker side
_M = None
_G = None


_DECL = None
_TOK = {}


def _init(model_args, gcache, dcache=None):
    global _M, _G, _DECL
    _M = hdrmodel.load(*model_args)
    with open(gcache, "rb") as f:
        _G = pickle.load(f)
    if dcache and os.path.exists(dcache):
        with open(dcache, "rb") as f:
            _DECL = pickle.load(f)


def build_decl(m, dcache):
    """header id -> frozenset of names whose mention in a .cpp means it uses the header."""
    if os.path.exists(dcache) and os.path.getmtime(dcache) > os.path.getmtime(m.build_dir + "/.ninja_deps"):
        with open(dcache, "rb") as f:
            return pickle.load(f)
    import declnames
    raw = {}
    mcount = collections.Counter()
    for i in range(len(m.paths)):
        if m.kind[i] == "ext":
            continue
        t, mem = declnames.decl_names(m.paths[i])
        raw[i] = (t, mem)
        mcount.update(mem)
    generic = {"get", "set", "update", "clear", "size", "begin", "end", "name", "value", "type",
               "data", "reset", "apply", "create", "remove", "insert", "isValid", "setValue",
               "getValue", "execute", "Restore", "Save", "onChanged"}
    decl = {}
    for i, (t, mem) in raw.items():
        d = set(t)
        d.update(x for x in mem if mcount[x] <= 2 and len(x) >= 5 and x not in generic)
        decl[i] = frozenset(d)
    with open(dcache, "wb") as f:
        pickle.dump(decl, f, protocol=pickle.HIGHEST_PROTOCOL)
    return decl


_IDRX = re.compile(r"[A-Za-z_]\w*")


def tu_tokens(ti):
    t = _TOK.get(ti)
    if t is None:
        src = _M.tus[ti]["src"]
        txt = ""
        for p in (src, os.path.splitext(src)[0] + ".h"):
            try:
                with open(p, encoding="utf-8", errors="replace") as f:
                    txt += "\n".join(l for l in f.read().split("\n") if not l.lstrip().startswith("#include"))
            except OSError:
                pass
        t = frozenset(_IDRX.findall(txt))
        _TOK[ti] = t
    return t


def tu_edges(ti, cut=None, add_root=(), extra=None):
    """Return (adj dict node->children, reachable order list). node -1 = the TU source.
    cut: set of (parent, child) edges to drop; extra: {parent: [child...]} edges to add
    (kept only where the child is in the TU's real header set)."""
    deps = _M.tus[ti]["deps"]
    adj = {}
    order = [-1]
    seen = {-1}
    k = 0
    while k < len(order):
        n = order[k]
        k += 1
        lists = _G.src[ti] if n == -1 else _G.hdr.get(n, ())
        ch = []
        for cl in lists:
            for c in cl:
                if c in deps:
                    if c != n and not (cut and (n, c) in cut):
                        ch.append(c)
                    break
        if n == -1:
            ch.extend(add_root)
        if extra and n in extra:
            ch.extend(c for c in extra[n] if c in deps)
        adj[n] = ch
        for c in ch:
            if c not in seen:
                seen.add(c)
                order.append(c)
    return adj, order


def dominators(adj, order):
    # Cooper-Harvey-Kennedy on the reachable graph rooted at -1
    rpo = []
    seen = {-1}
    stack = [(-1, iter(adj.get(-1, ())))]
    post = []
    while stack:
        n, it = stack[-1]
        for c in it:
            if c not in seen:
                seen.add(c)
                stack.append((c, iter(adj.get(c, ()))))
                break
        else:
            stack.pop()
            post.append(n)
    rpo = post[::-1]
    idx = {n: i for i, n in enumerate(rpo)}
    preds = collections.defaultdict(list)
    for n in rpo:
        for c in adj.get(n, ()):
            preds[c].append(n)
    idom = {-1: -1}
    changed = True
    while changed:
        changed = False
        for n in rpo[1:]:
            new = None
            for p in preds[n]:
                if p in idom:
                    if new is None:
                        new = p
                    else:
                        a, b = p, new
                        while a != b:
                            while idx[a] > idx[b]:
                                a = idom[a]
                            while idx[b] > idx[a]:
                                b = idom[b]
                        new = a
            if new is not None and idom.get(n) != new:
                idom[n] = new
                changed = True
    return idom, preds


def _analyze_chunk(args):
    tis, targets = args
    targets = set(targets)
    par = collections.defaultdict(collections.Counter)
    dom = collections.defaultdict(collections.Counter)
    direct = collections.Counter()
    present = collections.Counter()
    unresolved = collections.Counter()
    famtu = collections.Counter()
    famdirect = collections.Counter()
    famsole = collections.defaultdict(collections.Counter)
    famgw = collections.defaultdict(collections.Counter)
    famdom = collections.defaultdict(collections.Counter)
    fam_ms = collections.defaultdict(list)
    for ti in tis:
        tu = _M.tus[ti]
        adj, order = tu_edges(ti)
        reach = set(order)
        idom, preds = dominators(adj, order)
        rootch = set(adj.get(-1, ()))
        for h in targets & tu["deps"]:
            present[h] += 1
            if h not in reach:
                unresolved[h] += 1
                continue
            if h in rootch:
                direct[h] += 1
            for p in set(preds[h]):
                par[h][p] += 1
            d = idom.get(h, -1)
            while d != -1:
                dom[h][d] += 1
                d = idom.get(d, -1)
        # families
        gw = collections.defaultdict(set)
        for n in order:
            if n != -1 and _M.kind[n] == "ext":
                continue
            for c in adj.get(n, ()):
                f = _G.fam[c]
                if f:
                    gw[f].add(n)
        tfam = {_G.fam[h] for h in tu["deps"] if _G.fam[h]}
        for f in tfam:
            famtu[f] += 1
            fam_ms[f].append(tu["ms"] or 0)
            g = gw.get(f, set())
            if -1 in g:
                famdirect[f] += 1
            for n in g:
                if n != -1:
                    famgw[f][n] += 1
            hg = [n for n in g if n != -1]
            if len(g) == 1 and hg:
                famsole[f][hg[0]] += 1
            if g and -1 not in g:
                # common dominators of all gateways (incl. gateway itself)
                common = None
                for n in g:
                    s = {n}
                    d = idom.get(n, -1)
                    while d != -1:
                        s.add(d)
                        d = idom.get(d, -1)
                    common = s if common is None else common & s
                for d in common or ():
                    famdom[f][d] += 1
        for f in ("occt", "coin", "qt", "python", "pyside", "vtk", "smesh"):
            if f not in tfam:
                fam_ms["no-" + f].append(tu["ms"] or 0)
    return dict(par=par, dom=dom, direct=direct, present=present, unresolved=unresolved,
                famtu=famtu, famdirect=famdirect, famsole=famsole, famgw=famgw, famdom=famdom,
                fam_ms=fam_ms)


def _whatif_chunk(args):
    tis, cut, needs, child_ids, extra = args
    # needs: a regex (applies to every cut child) or {child id: regex}
    if isinstance(needs, dict):
        rxs = {c: re.compile(r) for c, r in needs.items()}
    elif needs:
        rxs = {c: re.compile(needs) for c in child_ids}
    else:
        rxs = {}
    rx = None
    lost = collections.defaultdict(list)
    kept_by_need = 0
    readded = collections.Counter()
    lost_bytes = {}
    for ti in tis:
        tu = _M.tus[ti]
        if not (child_ids & tu["deps"]):
            continue
        adj0, order0 = tu_edges(ti)
        r0 = set(order0)
        add = set()
        if rxs:
            txt = ""
            for p in (tu["src"], os.path.splitext(tu["src"])[0] + ".h"):
                try:
                    with open(p, encoding="utf-8", errors="replace") as f:
                        txt += f.read()
                except OSError:
                    pass
            for c, r in rxs.items():
                if c in r0 and r.search(txt):
                    add.add(c)
            if add:
                kept_by_need += 1
        toks = tu_tokens(ti) if _DECL is not None else frozenset()
        while True:
            adj1, order1 = tu_edges(ti, cut=cut, add_root=tuple(add), extra=extra)
            r1 = set(order1)
            gone = [h for h in r0 - r1 if h != -1 and _M.kind[h] != "ext"]
            # include-what-you-use: a .cpp that names something a lost header declares
            # would include that header itself
            newly = [h for h in gone if _DECL is not None and (_DECL.get(h, frozenset()) & toks)]
            if _DECL is not None:
                # Qt / OCCT / Coin headers are named after the class they declare
                for h in r0 - r1:
                    if h != -1 and _M.kind[h] == "ext" and _G.fam[h] in ("qt", "occt", "coin"):
                        stem = os.path.splitext(os.path.basename(_M.paths[h]))[0]
                        if stem in toks:
                            newly.append(h)
            if not newly:
                break
            add.update(newly)
            readded.update(newly)
        for h in gone:
            lost[h].append(ti)
        b = 0
        for h in r0 - r1:
            if h != -1:
                b += _size(h)
        lost_bytes[ti] = b
    return dict(lost=lost, kept=kept_by_need, readded=readded, lost_bytes=lost_bytes)


_SIZE = {}


def _size(h):
    v = _SIZE.get(h)
    if v is None:
        try:
            v = os.path.getsize(_M.paths[h])
        except OSError:
            v = 0
        _SIZE[h] = v
    return v


def chunks(n, k):
    return [list(range(i, n, k)) for i in range(k)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["analyze", "whatif", "parse"])
    ap.add_argument("build")
    ap.add_argument("src")
    ap.add_argument("deps")
    ap.add_argument("--targets")
    ap.add_argument("--spec")
    ap.add_argument("--out")
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--churn")
    ap.add_argument("--sec-per-mb", type=float, default=1.0,
                    help="compile seconds per MB of headers (parse_cost.py fits it)")
    ap.add_argument("--no-iwyu", action="store_true",
                    help="do not re-add a lost header to a .cpp that names what it declares")
    a = ap.parse_args()
    margs = (a.build, a.src, a.deps)
    m = hdrmodel.load(*margs)
    gcache = a.deps + ".graph.pkl"
    g = build_graph(m, gcache)
    if a.cmd == "parse":
        print("parsed", len(g.hdr), "headers", len(g.src), "sources")
        return
    disp = {hdrmodel.display(m, i).lower(): i for i in range(len(m.paths))}

    def find(p):
        p = p.strip().replace("\\", "/").lower()
        if p == "<src>":
            return -1
        if p in disp:
            return disp[p]
        raise SystemExit("unknown header " + p)

    dcache = a.deps + ".decl.pkl"
    if a.cmd == "whatif" and not a.no_iwyu:
        build_decl(m, dcache)
    pool = mp.Pool(min(a.jobs, 6), initializer=_init,
                   initargs=(margs, gcache, None if a.no_iwyu else dcache))
    if a.cmd == "analyze":
        targets = [find(l) for l in open(a.targets, encoding="utf-8") if l.strip()]
        acc = {}
        for r in pool.imap_unordered(_analyze_chunk, [(c, targets) for c in chunks(len(m.tus), 24)]):
            # merge
            for k, v in r.items():
                if k not in acc:
                    acc[k] = v
                elif isinstance(v, collections.Counter):
                    acc[k].update(v)
                else:
                    for kk, vv in v.items():
                        if isinstance(vv, list):
                            acc[k][kk].extend(vv)
                        else:
                            acc[k][kk].update(vv)
        D = lambda i: "<src>" if i == -1 else hdrmodel.display(m, i)  # noqa: E731
        out = {"targets": {}, "families": {}}
        for h in targets:
            out["targets"][D(h)] = dict(
                present=acc["present"][h], unresolved=acc["unresolved"][h], direct=acc["direct"][h],
                parents=[(D(p), c) for p, c in acc["par"][h].most_common(15)],
                dominators=[(D(p), c) for p, c in acc["dom"][h].most_common(15)])
        for f, n in acc["famtu"].items():
            ms = acc["fam_ms"].get(f, [])
            nms = acc["fam_ms"].get("no-" + f, [])
            out["families"][f] = dict(
                tus=n, direct_in_cpp=acc["famdirect"][f],
                mean_ms=sum(ms) / max(1, len(ms)), mean_ms_without=sum(nms) / max(1, len(nms)),
                cpu_min=sum(ms) / 60000,
                sole_gateway=[(D(p), c) for p, c in acc["famsole"][f].most_common(25)],
                gateways=[(D(p), c) for p, c in acc["famgw"][f].most_common(25)],
                umbrella=[(D(p), c) for p, c in acc["famdom"][f].most_common(25)])
        with open(a.out, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=1)
        print("wrote", a.out)
    else:
        churn = {}
        if a.churn:
            for line in open(a.churn, encoding="utf-8"):
                c, p = line.rstrip("\n").split("\t", 1)
                churn[p.lower()] = int(c)
        props = []
        cur = None
        for line in open(a.spec, encoding="utf-8"):
            line = line.rstrip("\n")
            if not line.strip() or line.startswith("#"):
                if cur:
                    props.append(cur)
                cur = None
                continue
            k, v = line.split(":", 1)
            cur = cur or {"cut": [], "needs": None, "name": "", "add": []}
            if k == "name":
                cur["name"] = v.strip()
            elif k == "cut":
                p, c = v.split("->")
                cur["cut"].append((find(p), find(c)))
            elif k == "needs":
                cur["needs"] = v.strip()
            elif k.startswith("needs@"):
                if not isinstance(cur.get("needs"), dict):
                    cur["needs"] = {}
                cur["needs"][find(k[6:])] = v.strip()
            elif k == "add":
                p, c = v.split("->")
                cur["add"].append((find(p), find(c)))
            elif k == "note":
                cur["note"] = v.strip()
        if cur:
            props.append(cur)
        # base cost per header
        tus_of = collections.defaultdict(set)
        for ti, tu in enumerate(m.tus):
            for h in tu["deps"]:
                if m.kind[h] != "ext":
                    tus_of[h].add(ti)

        def cost(tis):
            cms = sum((m.tus[t]["ms"] or 0) for t in tis)
            links = set()
            for t in tis:
                links.update(m.tus[t]["links"])
            return cms, sum(m.link_ms.get(l, 0) for l in links), len(links)

        results = []
        for pr in props:
            cut = set(pr["cut"])
            child_ids = {c for _, c in cut}
            lost = collections.defaultdict(set)
            kept = 0
            extra = collections.defaultdict(list)
            for p_, c_ in pr["add"]:
                extra[p_].append(c_)
            extra = dict(extra)
            readded = collections.Counter()
            lost_bytes = {}
            for r in pool.imap_unordered(_whatif_chunk, [(c, cut, pr["needs"], child_ids, extra) for c in chunks(len(m.tus), 24)]):
                readded.update(r["readded"])
                lost_bytes.update(r["lost_bytes"])
                for h, l in r["lost"].items():
                    lost[h].update(l)
                kept += r["kept"]
            per = []
            wsum_before = wsum_after = 0.0
            for h, l in lost.items():
                d = hdrmodel.display(m, h)
                b = cost(tus_of[h])
                aft = cost(tus_of[h] - l)
                ch = churn.get(d.lower(), 0)
                wsum_before += ch * (b[0] + b[1])
                wsum_after += ch * (aft[0] + aft[1])
                per.append(dict(header=d, tus_before=len(tus_of[h]), tus_after=len(tus_of[h] - l),
                                cpu_before_min=round(b[0] / 60000, 1), cpu_after_min=round(aft[0] / 60000, 1),
                                links_before=b[2], links_after=aft[2], churn=ch))
            per.sort(key=lambda x: -(x["cpu_before_min"] - x["cpu_after_min"]))
            child_rows = [x for x in per if find(x["header"]) in child_ids]
            results.append(dict(name=pr["name"], cut=[("<src>" if p == -1 else hdrmodel.display(m, p), hdrmodel.display(m, c)) for p, c in cut],
                                needs=(pr["needs"] if not isinstance(pr["needs"], dict) else {hdrmodel.display(m, c): r for c, r in pr["needs"].items()}), tus_kept_by_needs=kept,
                                churn_weighted_cpu_h_saved_12mo=round((wsum_before - wsum_after) / 3600000, 1),
                                headers_affected=len(per), children=child_rows, top=per[:25],
                                note=pr.get("note", ""),
                                full_build_mb_less=round(sum(lost_bytes.values()) / 1e6, 1),
                                full_build_cpu_min_less=round(sum(lost_bytes.values()) / 1e6 * a.sec_per_mb / 60, 1),
                                readded_top=[(hdrmodel.display(m, h), n) for h, n in readded.most_common(10)]))
            mb = sum(lost_bytes.values()) / 1e6
            ntu = sum(1 for v in lost_bytes.values() if v)
            print(f"{pr['name']}: headers affected {len(per)}, churn-weighted saved "
                  f"{(wsum_before - wsum_after)/3600000:.1f} CPU-h/12mo; full build parses "
                  f"{mb:.0f} MB less over {ntu} TUs (~{mb * a.sec_per_mb / 60:.0f} CPU-min at "
                  f"{a.sec_per_mb} s/MB)")
            for x in per[:8]:
                if find(x["header"]) not in child_ids:
                    print(f"   also {x['header']}: TUs {x['tus_before']} -> {x['tus_after']}, "
                          f"CPU {x['cpu_before_min']} -> {x['cpu_after_min']} min (churn {x['churn']})")
            for x in child_rows:
                print(f"   child {x['header']}: TUs {x['tus_before']} -> {x['tus_after']}, "
                      f"CPU {x['cpu_before_min']} -> {x['cpu_after_min']} min")
        with open(a.out, "w", encoding="utf-8") as f:
            json.dump(results, f, indent=1)
    pool.close()


if __name__ == "__main__":
    mp.freeze_support()
    main()
