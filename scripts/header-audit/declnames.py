"""Rough 'what does this header declare' extraction, for an include-what-you-use estimate.

decl_names(path) -> (top, members)
  top     : names a user must spell to use the header: classes/structs/enums/unions defined at
            namespace scope, free functions, namespace-scope variables/constants, using/typedef
            aliases, #define macros, enumerators of unscoped namespace-scope enums.
  members : member-function names of classes defined in the header (filtered later to the
            distinctive ones -- declared in at most two headers -- so `obj->recompute()` counts
            as a use of the header that defines recompute()).
It is a heuristic, not a parser: good enough to tell "this .cpp mentions nothing the header
declares" from "it uses it".
"""
import re

KW = {"if", "for", "while", "switch", "return", "sizeof", "decltype", "alignof", "static_assert", "do",
      "operator", "noexcept", "explicit", "template", "typename", "const", "constexpr", "inline",
      "static", "virtual", "override", "final", "friend", "public", "private", "protected",
      "class", "struct", "enum", "union", "namespace", "using", "typedef", "extern", "void",
      "int", "bool", "char", "double", "float", "long", "short", "unsigned", "signed", "auto",
      "new", "delete", "this", "true", "false", "nullptr", "default", "throw", "catch", "try",
      "mutable", "volatile", "register", "consteval", "constinit", "requires", "concept", "std"}

_ident = re.compile(r"[A-Za-z_]\w*")


def _strip(text):
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        elif c == '"' or c == "'":
            # raw strings R"( ... )" are rare in headers; treat simply
            q = c
            j = i + 1
            while j < n and text[j] != q:
                if text[j] == "\\":
                    j += 1
                elif text[j] == "\n":
                    break
                j += 1
            out.append(q + q)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _is_macro(name):
    return name.isupper() and len(name) > 1


def decl_names(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            raw = f.read()
    except OSError:
        return set(), set()
    text = _strip(raw)
    top, members = set(), set()
    lines = []
    for line in text.split("\n"):
        s = line.strip()
        if s.startswith("#"):
            m = re.match(r"#\s*define\s+(\w+)", s)
            if m and not m.group(1).endswith("_H") and not m.group(1).endswith("_H_"):
                top.add(m.group(1))
            lines.append("")
        else:
            lines.append(line)
    text = "\n".join(lines)
    # strip continuation leftovers of multi-line macros: crude but fine
    stack = []  # kinds: ('ns',), ('class', name), ('enum', scoped), ('other',)
    buf = []

    def level():
        kinds = [k[0] for k in stack]
        if all(k == "ns" for k in kinds):
            return "top"
        if kinds and kinds[-1] == "class" and all(k in ("ns", "class") for k in kinds):
            return "class"
        return None

    def stmt(s, lv, ending):
        s = " ".join(s.split())
        if not s:
            return
        s = re.sub(r"^(public|private|protected)\s*:\s*", "", s)
        s = re.sub(r"\b(Q_OBJECT|Q_GADGET|TYPESYSTEM_HEADER\w*|PROPERTY_HEADER\w*|EXTENSION_PROPERTY_HEADER\w*|"
                   r"Q_DECLARE_\w+\([^)]*\)|Q_ENUM\([^)]*\)|Q_PROPERTY\([^)]*\))", " ", s).strip()
        s = re.sub(r"^template\s*<.*?>\s*", "", s)
        if not s:
            return
        if lv == "top":
            m = re.match(r"using\s+(\w+)\s*=", s)
            if m:
                top.add(m.group(1))
                return
            if s.startswith("typedef"):
                ids = _ident.findall(s)
                if ids:
                    top.add(ids[-1])
                return
            if s.startswith("using ") or s.startswith("namespace") or s.startswith("friend"):
                return
            if re.match(r"(class|struct|union|enum)\b[^({=]*$", s) and ending == ";":
                return  # forward declaration
            p = s.find("(")
            if p > 0:
                ids = _ident.findall(s[:p])
                if ids:
                    nm = ids[-1]
                    if nm not in KW and not _is_macro(nm):
                        top.add(nm)
                return
            m = re.search(r"(\w+)\s*(=|\[|$)", s)
            if m and m.group(1) not in KW and not _is_macro(m.group(1)):
                top.add(m.group(1))
        elif lv == "class":
            p = s.find("(")
            if p > 0:
                ids = _ident.findall(s[:p])
                if ids:
                    nm = ids[-1]
                    if nm not in KW and not _is_macro(nm) and "~" not in s[:p].split()[-1:]:
                        members.add(nm)

    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "{":
            head = "".join(buf)
            hs = " ".join(head.split())
            lv = level()
            if re.search(r'\bnamespace\b[\w\s:]*$', hs) or re.search(r'extern\s*""\s*$', hs) or hs.endswith('extern ""'):
                stack.append(("ns",))
            else:
                hs2 = re.sub(r"^template\s*<.*?>\s*", "", re.sub(r"^(public|private|protected)\s*:\s*", "", hs))
                m = re.match(r"(?:typedef\s+)?(class|struct|union)\s+(?:\w+Export\s+|alignas\([^)]*\)\s+|\[\[[^\]]*\]\]\s+)*(\w+)", hs2)
                me = re.match(r"enum\s+(class\s+|struct\s+)?(\w+)?", hs2)
                if m and "(" not in hs2.split(m.group(2), 1)[0]:
                    if lv == "top":
                        top.add(m.group(2))
                    stack.append(("class", m.group(2)))
                elif me:
                    if lv == "top" and me.group(2):
                        top.add(me.group(2))
                    stack.append(("enum", bool(me.group(1)), lv))
                else:
                    if lv in ("top", "class"):
                        stmt(head, lv, "{")
                    stack.append(("other",))
            buf = []
        elif c == "}":
            if stack:
                k = stack.pop()
                if k[0] == "enum" and not k[1] and k[2] == "top":
                    for item in "".join(buf).split(","):
                        ids = _ident.findall(item)
                        if ids:
                            top.add(ids[0])
            buf = []
        elif c == ";":
            lv = level()
            if lv in ("top", "class"):
                stmt("".join(buf), lv, ";")
            buf = []
        else:
            buf.append(c)
        i += 1
    top.discard("")
    return top, members


if __name__ == "__main__":
    import sys
    for p in sys.argv[1:]:
        t, mbr = decl_names(p)
        print(p)
        print("  top:", sorted(t)[:200])
        print("  members:", len(mbr), sorted(mbr)[:60])
