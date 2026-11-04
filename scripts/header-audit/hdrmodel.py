"""Load a Ninja build tree into a header-dependency model.

Inputs (all read-only):
  <build>/build.ninja      compile edges (obj -> source) and link edges (target -> objs/libs)
  <build>/.ninja_log       per-step start/end (ms); last entry per path wins
  deps dump                `ninja -t deps` output (header list per object, as the compiler
                           reported it via /showIncludes or -MD)

Produces a Model with:
  tus[i]        = dict(obj, src, ms, links=[link output ...], deps=frozenset(header ids))
  paths[id]     = display path of header id (original case), norm[id] = normalized key
  kind[id]      = 'fork' | 'gen' | 'ext'
  link_ms[out]  = duration of that link step

Usage as a library: m = load(build_dir, src_root, deps_file); pickle is cached next to
the deps file.
"""
import os
import pickle
import re
import sys


def _norm(p, build_dir):
    p = p.replace("\\", "/")
    if not re.match(r"^[A-Za-z]:/", p) and not p.startswith("/"):
        p = build_dir.replace("\\", "/") + "/" + p
    p = os.path.normpath(p).replace("\\", "/")
    return p


_tok = re.compile(r"(?:\$.|[^ $])+")


def _split_ninja(s):
    out = []
    for t in _tok.findall(s):
        t = t.replace("$ ", " ").replace("$:", ":").replace("$$", "$")
        out.append(t)
    return out


def parse_build_ninja(build_dir):
    compile_src = {}  # obj(norm lower) -> src (norm)
    link_inputs = {}  # out(norm lower) -> (rule, [inputs norm lower])
    with open(os.path.join(build_dir, "build.ninja"), encoding="utf-8", errors="replace") as f:
        buf = ""
        for line in f:
            line = line.rstrip("\r\n")
            if buf:
                line = buf + line.lstrip()
                buf = ""
            if line.endswith("$") and not line.endswith("$$"):
                buf = line[:-1]
                continue
            if not line.startswith("build "):
                continue
            # split outputs / rule+inputs on first unescaped ':'
            m = re.match(r"build ((?:\$.|[^:$])*):\s*(\S+)\s*(.*)$", line)
            if not m:
                continue
            outs = _split_ninja(m.group(1))
            rule = m.group(2)
            rest = m.group(3)
            order = ""
            if "||" in rest:
                rest, order = rest.split("||", 1)
            implicit = ""
            if " | " in " " + rest + " ":
                parts = re.split(r"(?:^|\s)\|(?:\s|$)", rest, maxsplit=1)
                rest = parts[0]
                implicit = parts[1] if len(parts) > 1 else ""
            ins = _split_ninja(rest)
            imps = _split_ninja(implicit)
            if "CXX_COMPILER" in rule or "C_COMPILER" in rule:
                if len(outs) >= 1 and outs[0].lower().endswith(".obj") and ins:
                    compile_src[_norm(outs[0], build_dir).lower()] = _norm(ins[0], build_dir)
            elif "LINKER" in rule:
                key = [_norm(o, build_dir).lower() for o in outs]
                link_inputs[key[0]] = (rule, [_norm(i, build_dir).lower() for i in ins + imps], key)
    return compile_src, link_inputs


def parse_ninja_log(build_dir):
    ms = {}
    with open(os.path.join(build_dir, ".ninja_log"), encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 5:
                continue
            try:
                s, e = int(parts[0]), int(parts[1])
            except ValueError:
                continue
            ms[_norm(parts[3], build_dir).lower()] = e - s
    return ms


def parse_deps(deps_file, build_dir):
    """Yield (obj_norm_lower, [header display paths])."""
    cur = None
    hdrs = []
    with open(deps_file, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.strip():
                continue
            if line[0] not in " \t":
                if cur is not None:
                    yield cur, hdrs
                tgt = line.split(": #deps", 1)[0]
                cur = _norm(tgt, build_dir).lower()
                hdrs = []
            else:
                hdrs.append(_norm(line.strip(), build_dir))
        if cur is not None:
            yield cur, hdrs


class Model:
    pass


def load(build_dir, src_root, deps_file, use_cache=True):
    build_dir = os.path.normpath(build_dir).replace("\\", "/")
    src_root = os.path.normpath(src_root).replace("\\", "/")
    cache = deps_file + ".model.pkl"
    if use_cache and os.path.exists(cache) and os.path.getmtime(cache) > os.path.getmtime(deps_file):
        with open(cache, "rb") as f:
            return pickle.load(f)
    compile_src, link_inputs = parse_build_ninja(build_dir)
    logms = parse_ninja_log(build_dir)

    m = Model()
    m.build_dir, m.src_root = build_dir, src_root
    m.paths, m.norm, m.kind = [], [], []
    ids = {}
    src_l = (src_root + "/src/").lower()
    third_l = (src_root + "/src/3rdparty/").lower()
    bsrc_l = (build_dir + "/src/").lower()
    bld_l = (build_dir + "/").lower()

    def hid(p):
        k = p.lower()
        i = ids.get(k)
        if i is None:
            i = len(m.paths)
            ids[k] = i
            m.paths.append(p)
            m.norm.append(k)
            if k.startswith(third_l):
                kd = "ext"
            elif k.startswith(src_l):
                kd = "fork"
            elif k.startswith(bsrc_l) and "/3rdparty/" not in k:
                kd = "gen"
            else:
                kd = "ext"
            m.kind.append(kd)
        return i

    # obj -> link outputs. Static libs propagate one level to their consumers.
    obj_links = {}
    lib_consumers = {}
    for out, (rule, ins, outs) in link_inputs.items():
        for i in ins:
            if i.endswith(".obj"):
                obj_links.setdefault(i, []).append(out)
            elif i.endswith(".lib"):
                lib_consumers.setdefault(i, []).append(out)
    static_out = {}
    for out, (rule, ins, outs) in link_inputs.items():
        if "STATIC_LIBRARY" in rule:
            static_out[out] = lib_consumers.get(out, [])

    m.link_ms = {}
    for out, (rule, ins, outs) in link_inputs.items():
        m.link_ms[out] = max([logms.get(o, 0) for o in outs] + [0])

    m.tus = []
    m.others = []  # non-compile steps with header deps (AUTOMOC/AUTOUIC timestamps)
    for obj, hdrs in parse_deps(deps_file, build_dir):
        if not obj.endswith(".obj") or obj not in compile_src:
            m.others.append(dict(target=obj, ms=logms.get(obj, 0),
                                 deps=frozenset(hid(h) for h in hdrs)))
            continue
        links = list(obj_links.get(obj, []))
        for l in list(links):
            links.extend(static_out.get(l, []))
        m.tus.append(
            dict(
                obj=obj,
                src=compile_src[obj],
                ms=logms.get(obj, None),
                links=sorted(set(links)),
                deps=frozenset(hid(h) for h in hdrs),
            )
        )
    m.ids = ids
    m.link_rule = {o: v[0] for o, v in link_inputs.items()}
    with open(cache, "wb") as f:
        pickle.dump(m, f, protocol=pickle.HIGHEST_PROTOCOL)
    return m


def display(m, i):
    p = m.paths[i]
    if m.kind[i] == "fork":
        return p[len(m.src_root) + 1 :]
    if m.kind[i] == "gen":
        return "<build>/" + p[len(m.build_dir) + 1 :]
    return p


def tu_display(m, tu):
    s = tu["src"]
    if s.lower().startswith(m.src_root.lower() + "/"):
        return s[len(m.src_root) + 1 :]
    if s.lower().startswith(m.build_dir.lower() + "/"):
        return "<build>/" + s[len(m.build_dir) + 1 :]
    return s


if __name__ == "__main__":
    b, s, d = sys.argv[1:4]
    m = load(b, s, d, use_cache=False)
    print("TUs", len(m.tus), "headers", len(m.paths))
